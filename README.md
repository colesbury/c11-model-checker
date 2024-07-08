CDSChecker models for Python
============================

This is a fork of CDSChecker, a model checker for C11/C++11 which
exhaustively explores the behaviors of code under the C/C++ memory model.

See https://plrg.ics.uci.edu/software_page/42-2/

Building
========

1. `git clone https://github.com/colesbury/c11-model-checker.git`
2. `make -j`

Running model tests
===================

Run, e.g. `./run.sh test/qsbr.o`

Current models
==============

QSBR
----
`./run.sh test/qsbr.o`

A simplified model of QSBR.

Dicts
-----
`./run.sh test/dict_value.o`

Simulate dictionary updates.

Seq Lock
--------
`./run.sh test/seqlock.o -m 10`

Sequence locks. Used in updating type caches. Note that the test requires `-m` (liveness) to avoid an infinite loop in `_PySeqLock_BeginRead`. Larger values result in more executions. A value of `-m 1` seems sufficient to catch interesting bugs, but I'm using `-m 10` to be conservative.
