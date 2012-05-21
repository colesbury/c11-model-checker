#ifndef __SNAPINTERFACE_H
#define __SNAPINTERFACE_H
#include "snapshot.h"
#include "mymemory.h"
#include <vector>
#include <utility>
#include <string>
#include <map>
#include <set>
#include "snapshot.h"
#include "libthreads.h"

class snapshotStack;
typedef std::basic_string<char, std::char_traits<char>, MyAlloc<char> > MyString;

void SnapshotGlobalSegments();

struct stackEntry {
  struct stackEntry *next;
  snapshot_id snapshotid;
  int index;
};

class snapshotStack {
 public:
  MEMALLOC
  snapshotStack( );
  ~snapshotStack();
  int backTrackBeforeStep(int seq_index);
  void snapshotStep(int seq_index);

 private: 
  struct stackEntry * stack;
};

/* Not sure what it even means to have more than one snapshot object,
   so let's just make a global reference to it.*/

extern snapshotStack * snapshotObject;
#endif