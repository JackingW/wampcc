#
# Copyright (c) 2017 Darren Smith
#
# wampcc is free software; you can redistribute it and/or modify
# it under the terms of the MIT license. See LICENSE for details.
#


## This is an end-user makefile, which can be used to compile the examples
## against an installed version of wampcc.  It is not used by the autotool
## systems to build the examples during configure & make. Rather, use this
## makefile to build the examples once wampcc has successfully been compiled and
## installed.


# comment-out these two lines to link to shared libs
STATIC_LINK_BEGIN=-Wl,-Bstatic
STATIC_LINK_END=-Wl,-Bdynamic

ifeq ($(WAMPCC_HOME),)
 $(error WAMPCC_HOME undefined - set WAMPCC_HOME to where wampcc was make-installed)
endif

ifeq ($(LIBUV_HOME),)
 $(error LIBUV_HOME undefined - set LIBUV_HOME to where libuv was make-installed)
endif

ifeq ($(JANSSON_HOME),)
 $(error JANSSON_HOME undefined - set JANSSON_HOME to where jansson was make-installed)
endif

WAMPCC_INC=-I$(WAMPCC_HOME)/include
WAMPCC_LIBS=-L$(WAMPCC_HOME)/lib -lwampcc -lwampcc_json

LIBUV_INC=-I$(LIBUV_HOME)/include
LIBUV_LIBS=-L$(LIBUV_HOME)/lib -luv

JANSSON_LIBS=-L$(JANSSON_HOME)/lib -ljansson

CXXFLAGS += -MMD -MP
CXXFLAGS += -Wall -O0 -g3 -ggdb -std=c++11 -pthread   $(WAMPCC_INC) $(JALSON_INC) $(LIBUV_INC)

LDLIBS += $(STATIC_LINK_BEGIN) $(WAMPCC_LIBS) $(LIBUV_LIBS) $(JANSSON_LIBS) $(STATIC_LINK_END) -lcrypto -lpthread -lssl -lrt


# Note, some programs (eg wampcc_tester.cc) cannot be built against a standalone
# wampcc installation because they depend on internal files (eg wampcc/utils.h)

all_srcs := $(shell find . -name \*.cc | \grep -v wampcc_tester.cc)
app_srcs := $(shell find . -name \*.cc | \grep -v wampcc_tester.cc)

all   : apps
apps  : $(app_srcs:%.cc=%)
test  : obj/test/main ; @$<
clean : ; rm -f $(app_srcs:%.cc=%) $(app_srcs:%.cc=%.d)

#-include $(all_srcs:%.cc=%.d)
.PRECIOUS : %.o

%.o            : %.cc ; $(COMPILE.cpp) $(OUTPUT_OPTION) $<
%              : %.o ; @$(LINK.cpp) $(OUTPUT_OPTION) $^  $(LDLIBS)
