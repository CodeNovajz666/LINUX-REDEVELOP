# Convenience wrapper around build.py (which does the real work).
# PYTHON=python on Windows, python3 on Linux/macOS.

PYTHON ?= python

.PHONY: all run test test1 shell clean

all:
	$(PYTHON) build.py

run:
	$(PYTHON) build.py run

test:
	$(PYTHON) build.py test -smp 2

test1:
	$(PYTHON) build.py test -smp 1

shell:
	$(PYTHON) tools/test_shell.py

clean:
	$(PYTHON) build.py clean
