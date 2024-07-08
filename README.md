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

- `test/qsbr.c`: simplified model of QSBR
- `test/dict_value.c`: simplified model of Python dicts demonstrating need for release ordering
