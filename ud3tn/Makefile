###############################################################################
# Default Commands
###############################################################################

.PHONY: all
all: posix

.PHONY: ud3tn
ud3tn: posix

.PHONY: clean
clean::
	$(RM) -rf build/

###############################################################################
# Execution and Deployment
###############################################################################

.PHONY: run-posix
run-posix: posix
	build/posix/ud3tn

.PHONY: run-unittest-posix
run-unittest-posix: unittest-posix
	build/posix/testud3tn


.PHONY: run-unittest-posix-with-coverage
run-unittest-posix-with-coverage:
	$(MAKE) run-unittest-posix coverage=yes && geninfo build/posix -b . -o ./coverage1.info && genhtml coverage1.info -o build/coverage && echo "Coverage report has been generated in 'file://$$(pwd)/build/coverage/index.html'"


###############################################################################
# Tools
###############################################################################

.PHONY: gdb-posix
gdb-posix: posix
	$(TOOLCHAIN_POSIX)gdb build/posix/ud3tn

.PHONY: aap2-proto-headers
aap2-proto-headers:
	python3 external/nanopb/generator/nanopb_generator.py -Icomponents --output-dir=generated --error-on-unmatched aap2/aap2.proto
	protoc -Icomponents/aap2 --python_out=python-ud3tn-utils/ud3tn_utils/aap2/generated/ aap2.proto

.PHONY: storage-agent-proto-headers
storage-agent-proto-headers:
	python3 external/nanopb/generator/nanopb_generator.py -Icomponents --output-dir=generated --error-on-unmatched agents/storage/storage_agent.proto
	protoc -Icomponents/agents/storage --python_out=python-ud3tn-utils/ud3tn_utils/storage_agent/generated/ storage_agent.proto

###############################################################################
# Docs
###############################################################################

.PHONY: protoc-gen-doc
protoc-gen-doc:
	protoc --doc_out=doc/references/protobuf --doc_opt=doc/references/protobuf/protoc-gen-doc-markdown.tmpl,index.md components/aap2/aap2.proto components/agents/storage/storage_agent.proto

.PHONY: doc
doc: protoc-gen-doc
	mkdocs build --site-dir result/

###############################################################################
# Tests
###############################################################################

.PHONY: integration-test
integration-test:
	pytest test/integration

.PHONY: integration-test-tcpspp
integration-test-tcpspp:
	CLA=tcpspp pytest test/integration

.PHONY: integration-test-tcpcl
integration-test-tcpcl:
	CLA=tcpcl pytest test/integration

.PHONY: integration-test-mtcp
integration-test-mtcp:
	CLA=mtcp pytest test/integration


# Directory for the virtual Python envionment
VENV := .venv

ifeq "$(verbose)" "yes"
  PIP = pip
else
  PIP = pip -q
  GET_PIP += > /dev/null
endif

.PHONY: virtualenv
virtualenv:
	@echo "Create virtualenv in $(VENV)/ ..."
	@python3 -m venv $(VENV)
	@echo "Install/update dependencies..."
	. $(VENV)/bin/activate && $(MAKE) update-virtualenv
	@echo
	@echo "=> To activate the virtualenv, source $(VENV)/bin/activate"
	@echo "   or use environment-setup tools like"
	@echo "     - virtualenv"
	@echo "     - virtualenvwrapper"
	@echo "     - direnv"

.PHONY: update-virtualenv
update-virtualenv:
	@echo "Update pip..."
	$(PIP) install -U pip
	@echo "Install local dependencies to site-packages..."
	$(PIP) install -e ./pyd3tn
	$(PIP) install -e ./python-ud3tn-utils
	@echo "Install additional dependencies ..."
	$(PIP) install -U -r ./test/integration/requirements.txt
	$(PIP) install -U -r ./tools/analysis/requirements.txt

###############################################################################
# Code Quality Tests (and Release Tool)
###############################################################################

.PHONY: check-style
check-style:
	bash ./tools/analysis/stylecheck.sh

.PHONY: clang-check-posix
clang-check-posix: ccmds-posix
	bash ./tools/analysis/clang-check.sh clang-check "posix"

.PHONY: clang-tidy-posix
clang-tidy-posix: ccmds-posix
	bash ./tools/analysis/clang-check.sh "clang-tidy --use-color" "posix"

###############################################################################
# Flags
###############################################################################

-include config.mk

CPPFLAGS += -Wall

ifeq "$(type)" "release"
  CPPFLAGS += -O2
else
  CPPFLAGS += -g -O0 -DDEBUG
endif

ifneq "$(wextra)" "no"
  ifeq "$(wextra)" "all"
    CPPFLAGS += -Wextra -Wconversion -Wundef -Wshadow -Wsign-conversion -Wformat-security
  else
    CPPFLAGS += -Wextra -Wno-error=extra -Wno-unused-parameter
    ifneq ($(TOOLCHAIN),clang)
      CPPFLAGS += -Wno-override-init -Wno-unused-but-set-parameter
    endif
  endif
endif

ifeq "$(werror)" "yes"
  CPPFLAGS += -Werror
endif

ifneq "$(verbose)" "yes"
  Q := @
  quiet := quiet_
  MAKEFLAGS += --no-print-directory
endif

ifeq "$(sanitize-strict)" "yes"
  sanitize ?= yes
  ARCH_FLAGS += -fno-sanitize-recover=address,undefined
  ifeq "$(TOOLCHAIN)" "clang"
    ARCH_FLAGS += -fno-sanitize-recover=unsigned-integer-overflow,implicit-conversion,local-bounds
  endif
endif

ifeq "$(sanitize)" "yes"
  ARCH_FLAGS += -fsanitize=address -fno-omit-frame-pointer -fsanitize=undefined
  ifeq "$(TOOLCHAIN)" "clang"
    ARCH_FLAGS += -fsanitize=unsigned-integer-overflow,implicit-conversion,local-bounds
  endif
else
  ifeq "$(TOOLCHAIN)" "clang"
    ifeq "$(sanitize)" "memory"
      ARCH_FLAGS += -fsanitize=memory -fsanitize-memory-track-origins
      ifeq "$(sanitize-strict)" "yes"
        ARCH_FLAGS += -fno-sanitize-recover=memory
      endif
    endif
    ifeq "$(sanitize)" "thread"
      ARCH_FLAGS += -fsanitize=thread
      ifeq "$(sanitize-strict)" "yes"
        ARCH_FLAGS += -fno-sanitize-recover=thread
      endif
    endif
  endif
endif

ifeq "$(coverage)" "yes"
  ARCH_FLAGS += --coverage
endif

###############################################################################
# uD3TN-Builds
###############################################################################

.PHONY: posix posix-lib posix-all unittest-posix ccmds-posix

ifndef PLATFORM

posix:
	@$(MAKE) PLATFORM=posix posix

posix-lib:
	@$(MAKE) PLATFORM=posix posix-lib

posix-all:
	@$(MAKE) PLATFORM=posix posix posix-lib

unittest-posix:
	@$(MAKE) PLATFORM=posix unittest-posix

data-decoder:
	@$(MAKE) PLATFORM=posix data-decoder

ccmds-posix:
	@$(MAKE) PLATFORM=posix build/posix/compile_commands.json

else # ifndef PLATFORM

include mk/$(PLATFORM).mk
include mk/build.mk

posix: build/posix/ud3tn
posix-lib: build/posix/libud3tn.so build/posix/libud3tn.a
posix-all: posix posix-lib
data-decoder: build/posix/ud3tndecode
unittest-posix: build/posix/testud3tn
ccmds-posix: build/posix/compile_commands.json

endif # ifndef PLATFORM
