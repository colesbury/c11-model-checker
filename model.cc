#include <stdio.h>
#include <algorithm>

#include "model.h"
#include "action.h"
#include "nodestack.h"
#include "schedule.h"
#include "snapshot-interface.h"
#include "common.h"
#include "clockvector.h"
#include "cyclegraph.h"
#include "promise.h"
#include "datarace.h"

#define INITIAL_THREAD_ID	0

ModelChecker *model;

/** @brief Constructor */
ModelChecker::ModelChecker(struct model_params params) :
	/* Initialize default scheduler */
	scheduler(new Scheduler()),
	num_executions(0),
	params(params),
	diverge(NULL),
	action_trace(new action_list_t()),
	thread_map(new HashTable<int, Thread *, int>()),
	obj_map(new HashTable<const void *, action_list_t, uintptr_t, 4>()),
	obj_thrd_map(new HashTable<void *, std::vector<action_list_t>, uintptr_t, 4 >()),
	promises(new std::vector<Promise *>()),
	lazy_sync_with_release(new HashTable<void *, std::list<ModelAction *>, uintptr_t, 4>()),
	thrd_last_action(new std::vector<ModelAction *>(1)),
	node_stack(new NodeStack()),
	mo_graph(new CycleGraph()),
	failed_promise(false),
	too_many_reads(false),
	asserted(false)
{
	/* Allocate this "size" on the snapshotting heap */
	priv = (struct model_snapshot_members *)calloc(1, sizeof(*priv));
	/* First thread created will have id INITIAL_THREAD_ID */
	priv->next_thread_id = INITIAL_THREAD_ID;

	lazy_sync_size = &priv->lazy_sync_size;
}

/** @brief Destructor */
ModelChecker::~ModelChecker()
{
	for (int i = 0; i < get_num_threads(); i++)
		delete thread_map->get(i);
	delete thread_map;

	delete obj_thrd_map;
	delete obj_map;
	delete action_trace;

	for (unsigned int i = 0; i < promises->size(); i++)
		delete (*promises)[i];
	delete promises;

	delete lazy_sync_with_release;

	delete thrd_last_action;
	delete node_stack;
	delete scheduler;
	delete mo_graph;
}

/**
 * Restores user program to initial state and resets all model-checker data
 * structures.
 */
void ModelChecker::reset_to_initial_state()
{
	DEBUG("+++ Resetting to initial state +++\n");
	node_stack->reset_execution();
	failed_promise = false;
	too_many_reads = false;
	reset_asserted();
	snapshotObject->backTrackBeforeStep(0);
}

/** @returns a thread ID for a new Thread */
thread_id_t ModelChecker::get_next_id()
{
	return priv->next_thread_id++;
}

/** @returns the number of user threads created during this execution */
int ModelChecker::get_num_threads()
{
	return priv->next_thread_id;
}

/** @returns a sequence number for a new ModelAction */
modelclock_t ModelChecker::get_next_seq_num()
{
	return ++priv->used_sequence_numbers;
}

/**
 * Choose the next thread in the replay sequence.
 *
 * If the replay sequence has reached the 'diverge' point, returns a thread
 * from the backtracking set. Otherwise, simply returns the next thread in the
 * sequence that is being replayed.
 */
Thread * ModelChecker::get_next_replay_thread()
{
	thread_id_t tid;

	/* Have we completed exploring the preselected path? */
	if (diverge == NULL)
		return NULL;

	/* Else, we are trying to replay an execution */
	ModelAction *next = node_stack->get_next()->get_action();

	if (next == diverge) {
		Node *nextnode = next->get_node();
		/* Reached divergence point */
		if (nextnode->increment_promise()) {
			/* The next node will try to satisfy a different set of promises. */
			tid = next->get_tid();
			node_stack->pop_restofstack(2);
		} else if (nextnode->increment_read_from()) {
			/* The next node will read from a different value. */
			tid = next->get_tid();
			node_stack->pop_restofstack(2);
		} else if (nextnode->increment_future_value()) {
			/* The next node will try to read from a different future value. */
			tid = next->get_tid();
			node_stack->pop_restofstack(2);
		} else {
			/* Make a different thread execute for next step */
			Node *node = nextnode->get_parent();
			tid = node->get_next_backtrack();
			node_stack->pop_restofstack(1);
		}
		DEBUG("*** Divergence point ***\n");
		diverge = NULL;
	} else {
		tid = next->get_tid();
	}
	DEBUG("*** ModelChecker chose next thread = %d ***\n", tid);
	ASSERT(tid != THREAD_ID_T_NONE);
	return thread_map->get(id_to_int(tid));
}

/**
 * Queries the model-checker for more executions to explore and, if one
 * exists, resets the model-checker state to execute a new execution.
 *
 * @return If there are more executions to explore, return true. Otherwise,
 * return false.
 */
bool ModelChecker::next_execution()
{
	DBG();

	num_executions++;

	if (isfinalfeasible() || DBG_ENABLED())
		print_summary();

	if ((diverge = get_next_backtrack()) == NULL)
		return false;

	if (DBG_ENABLED()) {
		printf("Next execution will diverge at:\n");
		diverge->print();
	}

	reset_to_initial_state();
	return true;
}

ModelAction * ModelChecker::get_last_conflict(ModelAction *act)
{
	action_type type = act->get_type();

	switch (type) {
		case ATOMIC_READ:
		case ATOMIC_WRITE:
		case ATOMIC_RMW:
			break;
		default:
			return NULL;
	}
	/* linear search: from most recent to oldest */
	action_list_t *list = obj_map->get_safe_ptr(act->get_location());
	action_list_t::reverse_iterator rit;
	for (rit = list->rbegin(); rit != list->rend(); rit++) {
		ModelAction *prev = *rit;
		if (act->is_synchronizing(prev))
			return prev;
	}
	return NULL;
}

void ModelChecker::set_backtracking(ModelAction *act)
{
	ModelAction *prev;
	Node *node;
	Thread *t = get_thread(act->get_tid());

	prev = get_last_conflict(act);
	if (prev == NULL)
		return;

	node = prev->get_node()->get_parent();

	while (!node->is_enabled(t))
		t = t->get_parent();

	/* Check if this has been explored already */
	if (node->has_been_explored(t->get_id()))
		return;

	/* Cache the latest backtracking point */
	if (!priv->next_backtrack || *prev > *priv->next_backtrack)
		priv->next_backtrack = prev;

	/* If this is a new backtracking point, mark the tree */
	if (!node->set_backtrack(t->get_id()))
		return;
	DEBUG("Setting backtrack: conflict = %d, instead tid = %d\n",
			prev->get_tid(), t->get_id());
	if (DBG_ENABLED()) {
		prev->print();
		act->print();
	}
}

/**
 * Returns last backtracking point. The model checker will explore a different
 * path for this point in the next execution.
 * @return The ModelAction at which the next execution should diverge.
 */
ModelAction * ModelChecker::get_next_backtrack()
{
	ModelAction *next = priv->next_backtrack;
	priv->next_backtrack = NULL;
	return next;
}

/**
 * This is the heart of the model checker routine. It performs model-checking
 * actions corresponding to a given "current action." Among other processes, it
 * calculates reads-from relationships, updates synchronization clock vectors,
 * forms a memory_order constraints graph, and handles replay/backtrack
 * execution when running permutations of previously-observed executions.
 *
 * @param curr The current action to process
 * @return The next Thread that must be executed. May be NULL if ModelChecker
 * makes no choice (e.g., according to replay execution, combining RMW actions,
 * etc.)
 */
Thread * ModelChecker::check_current_action(ModelAction *curr)
{
	bool second_part_of_rmw = false;

	ASSERT(curr);

	if (curr->is_rmwc() || curr->is_rmw()) {
		ModelAction *tmp = process_rmw(curr);
		second_part_of_rmw = true;
		delete curr;
		curr = tmp;
	} else {
		ModelAction *tmp = node_stack->explore_action(curr);
		if (tmp) {
			/* Discard duplicate ModelAction; use action from NodeStack */
			/* First restore type and order in case of RMW operation */
			if (curr->is_rmwr())
				tmp->copy_typeandorder(curr);

			/* If we have diverged, we need to reset the clock vector. */
			if (diverge == NULL)
				tmp->create_cv(get_parent_action(tmp->get_tid()));

			delete curr;
			curr = tmp;
		} else {
			/*
			 * Perform one-time actions when pushing new ModelAction onto
			 * NodeStack
			 */
			curr->create_cv(get_parent_action(curr->get_tid()));
			/* Build may_read_from set */
			if (curr->is_read())
				build_reads_from_past(curr);
			if (curr->is_write())
				compute_promises(curr);
		}
	}

	/* Assign 'creation' parent */
	if (curr->get_type() == THREAD_CREATE) {
		Thread *th = (Thread *)curr->get_location();
		th->set_creation(curr);
	} else if (curr->get_type() == THREAD_JOIN) {
		Thread *wait, *join;
		wait = get_thread(curr->get_tid());
		join = (Thread *)curr->get_location();
		if (!join->is_complete())
			scheduler->wait(wait, join);
	} else if (curr->get_type() == THREAD_FINISH) {
		Thread *th = get_thread(curr->get_tid());
		while (!th->wait_list_empty()) {
			Thread *wake = th->pop_wait_list();
			scheduler->wake(wake);
		}
		th->complete();
	}

	/* Deal with new thread */
	if (curr->get_type() == THREAD_START)
		check_promises(NULL, curr->get_cv());

	/* Assign reads_from values */
	Thread *th = get_thread(curr->get_tid());
	uint64_t value = VALUE_NONE;
	bool updated = false;
	if (curr->is_read()) {
		while(true) {
			const ModelAction *reads_from = curr->get_node()->get_read_from();
			if (reads_from != NULL) {
				value = reads_from->get_value();
				/* Assign reads_from, perform release/acquire synchronization */
				curr->read_from(reads_from);
				if (!second_part_of_rmw)
					check_recency(curr,false);

				bool r_status=r_modification_order(curr,reads_from);

				if (!second_part_of_rmw&&!isfeasible()&&(curr->get_node()->increment_read_from()||!curr->get_node()->future_value_empty())) {
					mo_graph->rollbackChanges();
					too_many_reads=false;
					continue;
				}

				mo_graph->commitChanges();
				updated |= r_status;
			} else {
				/* Read from future value */
				value = curr->get_node()->get_future_value();
				curr->read_from(NULL);
				Promise *valuepromise = new Promise(curr, value);
				promises->push_back(valuepromise);
			}
			break;
		}
	} else if (curr->is_write()) {
		if (w_modification_order(curr))
			updated = true;
		if (resolve_promises(curr))
			updated = true;
		mo_graph->commitChanges();
	}

	if (updated)
		resolve_release_sequences(curr->get_location());

	th->set_return_value(value);

	/* Add action to list.  */
	if (!second_part_of_rmw)
		add_action_to_lists(curr);

	Node *currnode = curr->get_node();
	Node *parnode = currnode->get_parent();

	if (!parnode->backtrack_empty() || !currnode->read_from_empty() ||
	          !currnode->future_value_empty() || !currnode->promise_empty())
		if (!priv->next_backtrack || *curr > *priv->next_backtrack)
			priv->next_backtrack = curr;

	set_backtracking(curr);

	/* Do not split atomic actions. */
	if (curr->is_rmwr())
		return thread_current();
	/* The THREAD_CREATE action points to the created Thread */
	else if (curr->get_type() == THREAD_CREATE)
		return (Thread *)curr->get_location();
	else
		return get_next_replay_thread();
}

/** @returns whether the current partial trace must be a prefix of a
 * feasible trace. */
bool ModelChecker::isfeasibleprefix() {
	return promises->size() == 0 && *lazy_sync_size == 0;
}

/** @returns whether the current partial trace is feasible. */
bool ModelChecker::isfeasible() {
	return !mo_graph->checkForCycles() && !failed_promise && !too_many_reads;
}

/** Returns whether the current completed trace is feasible. */
bool ModelChecker::isfinalfeasible() {
	return isfeasible() && promises->size() == 0;
}

/** Close out a RMWR by converting previous RMWR into a RMW or READ. */
ModelAction * ModelChecker::process_rmw(ModelAction *act) {
	int tid = id_to_int(act->get_tid());
	ModelAction *lastread = get_last_action(tid);
	lastread->process_rmw(act);
	if (act->is_rmw())
		mo_graph->addRMWEdge(lastread->get_reads_from(), lastread);
	return lastread;
}

/**
 * Checks whether a thread has read from the same write for too many times
 * without seeing the effects of a later write.
 *
 * Basic idea:
 * 1) there must a different write that we could read from that would satisfy the modification order,
 * 2) we must have read from the same value in excess of maxreads times, and
 * 3) that other write must have been in the reads_from set for maxreads times.
 *
 * If so, we decide that the execution is no longer feasible.
 */
void ModelChecker::check_recency(ModelAction *curr, bool already_added) {
	if (params.maxreads != 0) {
		if (curr->get_node()->get_read_from_size() <= 1)
			return;

		//Must make sure that execution is currently feasible...  We could
		//accidentally clear by rolling back
		if (!isfeasible())
			return;

		std::vector<action_list_t> *thrd_lists = obj_thrd_map->get_safe_ptr(curr->get_location());
		int tid = id_to_int(curr->get_tid());

		/* Skip checks */
		if ((int)thrd_lists->size() <= tid)
			return;

		action_list_t *list = &(*thrd_lists)[tid];

		action_list_t::reverse_iterator rit = list->rbegin();
		/* Skip past curr */
		if (!already_added) {
			for (; (*rit) != curr; rit++)
				;
			/* go past curr now */
			rit++;
		}

		action_list_t::reverse_iterator ritcopy=rit;
		//See if we have enough reads from the same value
		int count=0;
		for (; count < params.maxreads; rit++,count++) {
			if (rit==list->rend())
				return;
			ModelAction *act = *rit;
			if (!act->is_read())
				return;
			if (act->get_reads_from() != curr->get_reads_from())
				return;
			if (act->get_node()->get_read_from_size() <= 1)
				return;
		}

		for (int i=0;i<curr->get_node()->get_read_from_size();i++) {
			//Get write
			const ModelAction * write=curr->get_node()->get_read_from_at(i);
			//Need a different write
			if (write==curr->get_reads_from())
				continue;

			/* Test to see whether this is a feasible write to read from*/
			r_modification_order(curr, write);
			bool feasiblereadfrom=isfeasible();
			mo_graph->rollbackChanges();

			if (!feasiblereadfrom)
				continue;
			rit=ritcopy;

			bool feasiblewrite=true;
			//new we need to see if this write works for everyone

			for (int loop=count;loop>0;loop--,rit++) {
				ModelAction *act=*rit;
				bool foundvalue=false;
				for(int j=0;j<act->get_node()->get_read_from_size();j++) {
					if (act->get_node()->get_read_from_at(i)==write) {
						foundvalue=true;
						break;
					}
				}
				if (!foundvalue) {
					feasiblewrite=false;
					break;
				}
			}
			if (feasiblewrite) {
				too_many_reads = true;
				return;
			}
		}
	}
}

/**
 * Updates the mo_graph with the constraints imposed from the current read.
 * @param curr The current action. Must be a read.
 * @param rf The action that curr reads from. Must be a write.
 * @return True if modification order edges were added; false otherwise
 */
bool ModelChecker::r_modification_order(ModelAction *curr, const ModelAction *rf)
{
	std::vector<action_list_t> *thrd_lists = obj_thrd_map->get_safe_ptr(curr->get_location());
	unsigned int i;
	bool added = false;
	ASSERT(curr->is_read());

	/* Iterate over all threads */
	for (i = 0; i < thrd_lists->size(); i++) {
		/* Iterate over actions in thread, starting from most recent */
		action_list_t *list = &(*thrd_lists)[i];
		action_list_t::reverse_iterator rit;
		for (rit = list->rbegin(); rit != list->rend(); rit++) {
			ModelAction *act = *rit;

			/* Include at most one act per-thread that "happens before" curr */
			if (act->happens_before(curr)) {
				if (act->is_read()) {
					const ModelAction *prevreadfrom = act->get_reads_from();
					if (prevreadfrom != NULL && rf != prevreadfrom) {
						mo_graph->addEdge(prevreadfrom, rf);
						added = true;
					}
				} else if (rf != act) {
					mo_graph->addEdge(act, rf);
					added = true;
				}
				break;
			}
		}
	}

	return added;
}

/** Updates the mo_graph with the constraints imposed from the current read. */
void ModelChecker::post_r_modification_order(ModelAction *curr, const ModelAction *rf)
{
	std::vector<action_list_t> *thrd_lists = obj_thrd_map->get_safe_ptr(curr->get_location());
	unsigned int i;
	ASSERT(curr->is_read());

	/* Iterate over all threads */
	for (i = 0; i < thrd_lists->size(); i++) {
		/* Iterate over actions in thread, starting from most recent */
		action_list_t *list = &(*thrd_lists)[i];
		action_list_t::reverse_iterator rit;
		ModelAction *lastact = NULL;

		/* Find last action that happens after curr */
		for (rit = list->rbegin(); rit != list->rend(); rit++) {
			ModelAction *act = *rit;
			if (curr->happens_before(act)) {
				lastact = act;
			} else
				break;
		}

			/* Include at most one act per-thread that "happens before" curr */
		if (lastact != NULL) {
			if (lastact->is_read()) {
				const ModelAction *postreadfrom = lastact->get_reads_from();
				if (postreadfrom != NULL&&rf != postreadfrom)
					mo_graph->addEdge(rf, postreadfrom);
			} else if (rf != lastact) {
				mo_graph->addEdge(rf, lastact);
			}
			break;
		}
	}
}

/**
 * Updates the mo_graph with the constraints imposed from the current write.
 * @param curr The current action. Must be a write.
 * @return True if modification order edges were added; false otherwise
 */
bool ModelChecker::w_modification_order(ModelAction *curr)
{
	std::vector<action_list_t> *thrd_lists = obj_thrd_map->get_safe_ptr(curr->get_location());
	unsigned int i;
	bool added = false;
	ASSERT(curr->is_write());

	if (curr->is_seqcst()) {
		/* We have to at least see the last sequentially consistent write,
			 so we are initialized. */
		ModelAction *last_seq_cst = get_last_seq_cst(curr->get_location());
		if (last_seq_cst != NULL) {
			mo_graph->addEdge(last_seq_cst, curr);
			added = true;
		}
	}

	/* Iterate over all threads */
	for (i = 0; i < thrd_lists->size(); i++) {
		/* Iterate over actions in thread, starting from most recent */
		action_list_t *list = &(*thrd_lists)[i];
		action_list_t::reverse_iterator rit;
		for (rit = list->rbegin(); rit != list->rend(); rit++) {
			ModelAction *act = *rit;

			/* Include at most one act per-thread that "happens before" curr */
			if (act->happens_before(curr)) {
				if (act->is_read())
					mo_graph->addEdge(act->get_reads_from(), curr);
				else
					mo_graph->addEdge(act, curr);
				added = true;
				break;
			} else if (act->is_read() && !act->is_synchronizing(curr) &&
			                             !act->same_thread(curr)) {
				/* We have an action that:
				   (1) did not happen before us
				   (2) is a read and we are a write
				   (3) cannot synchronize with us
				   (4) is in a different thread
				   =>
				   that read could potentially read from our write.
				 */
				if (act->get_node()->add_future_value(curr->get_value()) &&
						(!priv->next_backtrack || *act > *priv->next_backtrack))
					priv->next_backtrack = act;
			}
		}
	}

	return added;
}

/**
 * Finds the head(s) of the release sequence(s) containing a given ModelAction.
 * The ModelAction under consideration is expected to be taking part in
 * release/acquire synchronization as an object of the "reads from" relation.
 * Note that this can only provide release sequence support for RMW chains
 * which do not read from the future, as those actions cannot be traced until
 * their "promise" is fulfilled. Similarly, we may not even establish the
 * presence of a release sequence with certainty, as some modification order
 * constraints may be decided further in the future. Thus, this function
 * "returns" two pieces of data: a pass-by-reference vector of @a release_heads
 * and a boolean representing certainty.
 *
 * @todo Finish lazy updating, when promises are fulfilled in the future
 * @param rf The action that might be part of a release sequence. Must be a
 * write.
 * @param release_heads A pass-by-reference style return parameter.  After
 * execution of this function, release_heads will contain the heads of all the
 * relevant release sequences, if any exists
 * @return true, if the ModelChecker is certain that release_heads is complete;
 * false otherwise
 */
bool ModelChecker::release_seq_head(const ModelAction *rf,
                std::vector<const ModelAction *> *release_heads) const
{
	ASSERT(rf->is_write());
	if (!rf) {
		/* read from future: need to settle this later */
		return false; /* incomplete */
	}
	if (rf->is_release())
		release_heads->push_back(rf);
	if (rf->is_rmw()) {
		/* We need a RMW action that is both an acquire and release to stop */
		/** @todo Need to be smarter here...  In the linux lock
		 * example, this will run to the beginning of the program for
		 * every acquire. */
		if (rf->is_acquire() && rf->is_release())
			return true; /* complete */
		return release_seq_head(rf->get_reads_from(), release_heads);
	}
	if (rf->is_release())
		return true; /* complete */

	/* else relaxed write; check modification order for contiguous subsequence
	 * -> rf must be same thread as release */
	int tid = id_to_int(rf->get_tid());
	std::vector<action_list_t> *thrd_lists = obj_thrd_map->get_safe_ptr(rf->get_location());
	action_list_t *list = &(*thrd_lists)[tid];
	action_list_t::const_reverse_iterator rit;

	/* Find rf in the thread list */
	rit = std::find(list->rbegin(), list->rend(), rf);
	ASSERT(rit != list->rend());

	/* Find the last write/release */
	for (; rit != list->rend(); rit++)
		if ((*rit)->is_release())
			break;
	if (rit == list->rend()) {
		/* No write-release in this thread */
		return true; /* complete */
	}
	ModelAction *release = *rit;

	ASSERT(rf->same_thread(release));

	bool certain = true;
	for (unsigned int i = 0; i < thrd_lists->size(); i++) {
		if (id_to_int(rf->get_tid()) == (int)i)
			continue;
		list = &(*thrd_lists)[i];

		/* Can we ensure no future writes from this thread may break
		 * the release seq? */
		bool future_ordered = false;

		for (rit = list->rbegin(); rit != list->rend(); rit++) {
			const ModelAction *act = *rit;
			if (!act->is_write())
				continue;
			/* Reach synchronization -> this thread is complete */
			if (act->happens_before(release))
				break;
			if (rf->happens_before(act)) {
				future_ordered = true;
				continue;
			}

			/* Check modification order */
			if (mo_graph->checkReachable(rf, act)) {
				/* rf --mo--> act */
				future_ordered = true;
				continue;
			}
			if (mo_graph->checkReachable(act, release))
				/* act --mo--> release */
				break;
			if (mo_graph->checkReachable(release, act) &&
			              mo_graph->checkReachable(act, rf)) {
				/* release --mo-> act --mo--> rf */
				return true; /* complete */
			}
			certain = false;
		}
		if (!future_ordered)
			return false; /* This thread is uncertain */
	}

	if (certain)
		release_heads->push_back(release);
	return certain;
}

/**
 * A public interface for getting the release sequence head(s) with which a
 * given ModelAction must synchronize. This function only returns a non-empty
 * result when it can locate a release sequence head with certainty. Otherwise,
 * it may mark the internal state of the ModelChecker so that it will handle
 * the release sequence at a later time, causing @a act to update its
 * synchronization at some later point in execution.
 * @param act The 'acquire' action that may read from a release sequence
 * @param release_heads A pass-by-reference return parameter. Will be filled
 * with the head(s) of the release sequence(s), if they exists with certainty.
 * @see ModelChecker::release_seq_head
 */
void ModelChecker::get_release_seq_heads(ModelAction *act,
                std::vector<const ModelAction *> *release_heads)
{
	const ModelAction *rf = act->get_reads_from();
	bool complete;
	complete = release_seq_head(rf, release_heads);
	if (!complete) {
		/* add act to 'lazy checking' list */
		std::list<ModelAction *> *list;
		list = lazy_sync_with_release->get_safe_ptr(act->get_location());
		list->push_back(act);
		(*lazy_sync_size)++;
	}
}

/**
 * Attempt to resolve all stashed operations that might synchronize with a
 * release sequence for a given location. This implements the "lazy" portion of
 * determining whether or not a release sequence was contiguous, since not all
 * modification order information is present at the time an action occurs.
 *
 * @param location The location/object that should be checked for release
 * sequence resolutions
 * @return True if any updates occurred (new synchronization, new mo_graph edges)
 */
bool ModelChecker::resolve_release_sequences(void *location)
{
	std::list<ModelAction *> *list;
	list = lazy_sync_with_release->getptr(location);
	if (!list)
		return false;

	bool updated = false;
	std::list<ModelAction *>::iterator it = list->begin();
	while (it != list->end()) {
		ModelAction *act = *it;
		const ModelAction *rf = act->get_reads_from();
		std::vector<const ModelAction *> release_heads;
		bool complete;
		complete = release_seq_head(rf, &release_heads);
		for (unsigned int i = 0; i < release_heads.size(); i++) {
			if (!act->has_synchronized_with(release_heads[i])) {
				updated = true;
				act->synchronize_with(release_heads[i]);
			}
		}

		if (updated) {
			/* propagate synchronization to later actions */
			action_list_t::reverse_iterator it = action_trace->rbegin();
			while ((*it) != act) {
				ModelAction *propagate = *it;
				if (act->happens_before(propagate))
					/** @todo new mo_graph edges along with
					 * this synchronization? */
					propagate->synchronize_with(act);
			}
		}
		if (complete) {
			it = list->erase(it);
			(*lazy_sync_size)--;
		} else
			it++;
	}

	// If we resolved promises or data races, see if we have realized a data race.
	if (checkDataRaces()) {
		set_assert();
	}

	return updated;
}

/**
 * Performs various bookkeeping operations for the current ModelAction. For
 * instance, adds action to the per-object, per-thread action vector and to the
 * action trace list of all thread actions.
 *
 * @param act is the ModelAction to add.
 */
void ModelChecker::add_action_to_lists(ModelAction *act)
{
	int tid = id_to_int(act->get_tid());
	action_trace->push_back(act);

	obj_map->get_safe_ptr(act->get_location())->push_back(act);

	std::vector<action_list_t> *vec = obj_thrd_map->get_safe_ptr(act->get_location());
	if (tid >= (int)vec->size())
		vec->resize(priv->next_thread_id);
	(*vec)[tid].push_back(act);

	if ((int)thrd_last_action->size() <= tid)
		thrd_last_action->resize(get_num_threads());
	(*thrd_last_action)[tid] = act;
}

ModelAction * ModelChecker::get_last_action(thread_id_t tid)
{
	int nthreads = get_num_threads();
	if ((int)thrd_last_action->size() < nthreads)
		thrd_last_action->resize(nthreads);
	return (*thrd_last_action)[id_to_int(tid)];
}

/**
 * Gets the last memory_order_seq_cst action (in the total global sequence)
 * performed on a particular object (i.e., memory location).
 * @param location The object location to check
 * @return The last seq_cst action performed
 */
ModelAction * ModelChecker::get_last_seq_cst(const void *location)
{
	action_list_t *list = obj_map->get_safe_ptr(location);
	/* Find: max({i in dom(S) | seq_cst(t_i) && isWrite(t_i) && samevar(t_i, t)}) */
	action_list_t::reverse_iterator rit;
	for (rit = list->rbegin(); rit != list->rend(); rit++)
		if ((*rit)->is_write() && (*rit)->is_seqcst())
			return *rit;
	return NULL;
}

ModelAction * ModelChecker::get_parent_action(thread_id_t tid)
{
	ModelAction *parent = get_last_action(tid);
	if (!parent)
		parent = get_thread(tid)->get_creation();
	return parent;
}

/**
 * Returns the clock vector for a given thread.
 * @param tid The thread whose clock vector we want
 * @return Desired clock vector
 */
ClockVector * ModelChecker::get_cv(thread_id_t tid)
{
	return get_parent_action(tid)->get_cv();
}

/**
 * Resolve a set of Promises with a current write. The set is provided in the
 * Node corresponding to @a write.
 * @param write The ModelAction that is fulfilling Promises
 * @return True if promises were resolved; false otherwise
 */
bool ModelChecker::resolve_promises(ModelAction *write)
{
	bool resolved = false;
	for (unsigned int i = 0, promise_index = 0; promise_index < promises->size(); i++) {
		Promise *promise = (*promises)[promise_index];
		if (write->get_node()->get_promise(i)) {
			ModelAction *read = promise->get_action();
			read->read_from(write);
			r_modification_order(read, write);
			post_r_modification_order(read, write);
			promises->erase(promises->begin() + promise_index);
			resolved = true;
		} else
			promise_index++;
	}

	return resolved;
}

/**
 * Compute the set of promises that could potentially be satisfied by this
 * action. Note that the set computation actually appears in the Node, not in
 * ModelChecker.
 * @param curr The ModelAction that may satisfy promises
 */
void ModelChecker::compute_promises(ModelAction *curr)
{
	for (unsigned int i = 0; i < promises->size(); i++) {
		Promise *promise = (*promises)[i];
		const ModelAction *act = promise->get_action();
		if (!act->happens_before(curr) &&
				act->is_read() &&
				!act->is_synchronizing(curr) &&
				!act->same_thread(curr) &&
				promise->get_value() == curr->get_value()) {
			curr->get_node()->set_promise(i);
		}
	}
}

/** Checks promises in response to change in ClockVector Threads. */
void ModelChecker::check_promises(ClockVector *old_cv, ClockVector *merge_cv)
{
	for (unsigned int i = 0; i < promises->size(); i++) {
		Promise *promise = (*promises)[i];
		const ModelAction *act = promise->get_action();
		if ((old_cv == NULL || !old_cv->synchronized_since(act)) &&
				merge_cv->synchronized_since(act)) {
			//This thread is no longer able to send values back to satisfy the promise
			int num_synchronized_threads = promise->increment_threads();
			if (num_synchronized_threads == get_num_threads()) {
				//Promise has failed
				failed_promise = true;
				return;
			}
		}
	}
}

/**
 * Build up an initial set of all past writes that this 'read' action may read
 * from. This set is determined by the clock vector's "happens before"
 * relationship.
 * @param curr is the current ModelAction that we are exploring; it must be a
 * 'read' operation.
 */
void ModelChecker::build_reads_from_past(ModelAction *curr)
{
	std::vector<action_list_t> *thrd_lists = obj_thrd_map->get_safe_ptr(curr->get_location());
	unsigned int i;
	ASSERT(curr->is_read());

	ModelAction *last_seq_cst = NULL;

	/* Track whether this object has been initialized */
	bool initialized = false;

	if (curr->is_seqcst()) {
		last_seq_cst = get_last_seq_cst(curr->get_location());
		/* We have to at least see the last sequentially consistent write,
			 so we are initialized. */
		if (last_seq_cst != NULL)
			initialized = true;
	}

	/* Iterate over all threads */
	for (i = 0; i < thrd_lists->size(); i++) {
		/* Iterate over actions in thread, starting from most recent */
		action_list_t *list = &(*thrd_lists)[i];
		action_list_t::reverse_iterator rit;
		for (rit = list->rbegin(); rit != list->rend(); rit++) {
			ModelAction *act = *rit;

			/* Only consider 'write' actions */
			if (!act->is_write())
				continue;

			/* Don't consider more than one seq_cst write if we are a seq_cst read. */
			if (!act->is_seqcst() || !curr->is_seqcst() || act == last_seq_cst) {
				DEBUG("Adding action to may_read_from:\n");
				if (DBG_ENABLED()) {
					act->print();
					curr->print();
				}
				curr->get_node()->add_read_from(act);
			}

			/* Include at most one act per-thread that "happens before" curr */
			if (act->happens_before(curr)) {
				initialized = true;
				break;
			}
		}
	}

	if (!initialized) {
		/** @todo Need a more informative way of reporting errors. */
		printf("ERROR: may read from uninitialized atomic\n");
	}

	if (DBG_ENABLED() || !initialized) {
		printf("Reached read action:\n");
		curr->print();
		printf("Printing may_read_from\n");
		curr->get_node()->print_may_read_from();
		printf("End printing may_read_from\n");
	}

	ASSERT(initialized);
}

static void print_list(action_list_t *list)
{
	action_list_t::iterator it;

	printf("---------------------------------------------------------------------\n");
	printf("Trace:\n");

	for (it = list->begin(); it != list->end(); it++) {
		(*it)->print();
	}
	printf("---------------------------------------------------------------------\n");
}

void ModelChecker::print_summary()
{
	printf("\n");
	printf("Number of executions: %d\n", num_executions);
	printf("Total nodes created: %d\n", node_stack->get_total_nodes());

	scheduler->print();

	if (!isfinalfeasible())
		printf("INFEASIBLE EXECUTION!\n");
	print_list(action_trace);
	printf("\n");
}

/**
 * Add a Thread to the system for the first time. Should only be called once
 * per thread.
 * @param t The Thread to add
 */
void ModelChecker::add_thread(Thread *t)
{
	thread_map->put(id_to_int(t->get_id()), t);
	scheduler->add_thread(t);
}

void ModelChecker::remove_thread(Thread *t)
{
	scheduler->remove_thread(t);
}

/**
 * Switch from a user-context to the "master thread" context (a.k.a. system
 * context). This switch is made with the intention of exploring a particular
 * model-checking action (described by a ModelAction object). Must be called
 * from a user-thread context.
 * @param act The current action that will be explored. Must not be NULL.
 * @return Return status from the 'swap' call (i.e., success/fail, 0/-1)
 */
int ModelChecker::switch_to_master(ModelAction *act)
{
	DBG();
	Thread *old = thread_current();
	set_current_action(act);
	old->set_state(THREAD_READY);
	return Thread::swap(old, &system_context);
}

/**
 * Takes the next step in the execution, if possible.
 * @return Returns true (success) if a step was taken and false otherwise.
 */
bool ModelChecker::take_step() {
	Thread *curr, *next;

	if (has_asserted())
		return false;

	curr = thread_current();
	if (curr) {
		if (curr->get_state() == THREAD_READY) {
			ASSERT(priv->current_action);
			priv->nextThread = check_current_action(priv->current_action);
			priv->current_action = NULL;
			if (!curr->is_blocked() && !curr->is_complete())
				scheduler->add_thread(curr);
		} else {
			ASSERT(false);
		}
	}
	next = scheduler->next_thread(priv->nextThread);

	/* Infeasible -> don't take any more steps */
	if (!isfeasible())
		return false;

	if (next)
		next->set_state(THREAD_RUNNING);
	DEBUG("(%d, %d)\n", curr ? curr->get_id() : -1, next ? next->get_id() : -1);

	/* next == NULL -> don't take any more steps */
	if (!next)
		return false;
	/* Return false only if swap fails with an error */
	return (Thread::swap(&system_context, next) == 0);
}

/** Runs the current execution until threre are no more steps to take. */
void ModelChecker::finish_execution() {
	DBG();

	while (take_step());
}
