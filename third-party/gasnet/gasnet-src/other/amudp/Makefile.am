## Process this file with automake to produce Makefile.in	-*- makefile -*-
#   $Source: bitbucket.org:berkeleylab/gasnet.git/other/amudp/Makefile.am $
# Description: Makefile for AMUDP/GASNet 
# Copyright 2004, Dan Bonachea <bonachea@cs.berkeley.edu>
# Terms of use are as specified in license.txt
# NOTE: This Makefile is used to tie in with the GASNet build infrastructure
#       the stand-alone AMUDP product doesn't currently provide an automake framework

AUTOMAKE_OPTIONS = foreign 1.4 no-dependencies

apputils_flags = 

if BUILDCONFIG_DEBUG
  BUILDCONFIG_DEFINES = -DAMUDP_DEBUG=1
else
  BUILDCONFIG_DEFINES = -DAMUDP_NDEBUG=1
endif

if BLCR_ENABLED
  blcr_defines = -DAMUDP_BLCR_ENABLED=1 -DBLCR_BINDIR=\"@BLCR_BINDIR@\" @BLCR_CPPFLAGS@
  blcr_ldflags = @BLCR_LIBDIR@ @BLCR_LIBS@
else
  blcr_defines =
  blcr_ldflags =
endif

if USE_STRICT_PROTOTYPES
  strict_proto_flags = -DGASNET_USE_STRICT_PROTOTYPES
else
  strict_proto_flags = 
endif

CC  = @CC@
CFLAGS = @GASNET_OPT_CFLAGS@ @MISC_CFLAGS@ @MISC_CPPFLAGS@ @SYS_HEADER_BLD@ $(MANUAL_CFLAGS) $(apputils_flags) $(strict_proto_flags)
CXX = @CXX@
CXXFLAGS = @GASNET_OPT_CXXFLAGS@ @MISC_CXXFLAGS@ @MISC_CPPFLAGS@ @SYS_HEADER_BLD@ $(MANUAL_CXXFLAGS)
LDFLAGS = @LDFLAGS@ @cxxLDFLAGS@ $(MANUAL_LDFLAGS)

PERL = @PERL@

platform_defines = @GASNET_EXTRA_DEFINES@ $(BUILDCONFIG_DEFINES) @INTTYPES_DEFINES@ -DAMUDP_ENV_PREFIX=GASNET $(blcr_defines) $(MANUAL_DEFINES)

platform_libraries = -L$(top_builddir) -lgasnet_tools-seq @LIBS@ @GASNET_UDP_LIBS@ $(blcr_ldflags)

VPATH = $(srcdir)
testdir = $(srcdir)/../amxtests
altincdir = $(srcdir)/..
gasnet_tools = -I$(srcdir)/../.. -I../.. -DHAVE_GASNET_TOOLS
INCLUDES = -I$(srcdir) -I. -I$(altincdir) $(platform_defines) $(gasnet_tools)
compile = $(CC) $(CFLAGS) $(INCLUDES)
cxxcompile = $(CXX) $(CXXFLAGS) $(INCLUDES)
link = $(CXX) $(CXXFLAGS) $(LDFLAGS) $(INCLUDES) 
linkend = $(platform_libraries) $(MANUAL_LIBS)

if USE_UDP_CONDUIT
BUILT_SOURCES = amudp_portable_platform.h
lib_LIBRARIES = libamudp.a
bin_PROGRAMS = amudprun
endif
headers = \
	amudp.h			\
	amudp_const.h		\
	amudp_internal.h	\
	amudp_spmd.h		\
	exc.h			\
	sig.h			\
	sockaddr.h		\
	socket.h		\
	socklist.h		\
	sockutil.h		

allheaders = $(BUILT_SOURCES) $(headers)

libamudp_a_SOURCES = \
	amudp_cdefs.c		\
	amudp_ep.cpp		\
	amudp_reqrep.cpp	\
	amudp_spawn.cpp		\
	amudp_spmd.cpp		\
	exc.cpp			\
	sig.cpp			\
	socklist.cpp		\
	sockutil.cpp		

amudp_portable_platform.h: $(altincdir)/gasnet_portable_platform.h
	cp -f $< $@

libamudp_a_DEPENDENCIES = $(libamudp_a_SOURCES:%.cpp=%.o) $(libamudp_a_SOURCES:%.c=%.o)

$(libamudp_a_DEPENDENCIES): $(allheaders)

EXTRA_DIST = \
        $(headers)              \
	Makefile.tests  	\
	Makefile.titanium.in 	\
	amudprun.c              \
	license.txt

# ---------- test programs -------------
# all the test executables

testprograms =    		\
    testam        		\
    testbounce    		\
    testbulk      		\
    testlatency   		\
    testlatencyM   		\
    testping      		\
    testreduce			\
    testoutput      		\
    testgetput    		\
    testreadwrite 

tests: apputils.o $(testprograms)

test%: test%.o apputils.o libamudp.a
	$(link) -o $@ $< apputils.o -L. -lamudp $(linkend)

tests-clean:
	rm -f $(testprograms)

include Makefile.tests

test%.o : $(testdir)/test%.c $(testdir)/*.h $(allheaders)
	$(compile) -DAMUDP -I$(testdir) -c $< -o $@

apputils.o : $(testdir)/apputils.c $(testdir)/apputils.h $(allheaders)
	$(compile) -DAMUDP -I$(testdir) -c $(testdir)/apputils.c -o apputils.o

amudprun$(EXEEXT): amudprun.o $(headers) libamudp.a
	$(link) -o $@ $< -L. -lamudp $(linkend)

%.o: %.c $(allheaders)
	$(compile) -o $@ -c `test -e $(srcdir)/$< && echo "$(srcdir)/"`$<

%.o: %.cpp $(allheaders) $(top_builddir)/libgasnet_tools-seq.a
	$(cxxcompile) -o $@ -c `test -e $(srcdir)/$< && echo "$(srcdir)/"`$<

$(top_builddir)/libgasnet_tools-seq.a: force
	@cd $(top_builddir) && $(MAKE) libgasnet_tools-seq.a

if USE_UDP_CONDUIT
if GNU_NM
check-exports: libamudp.a
	@echo Checking libamudp exports...
	@lib=libamudp.a ; \
            @NM@ --defined-only $$lib |                                             \
	    @PERL@ -pe 's/ \.refptr\.//' |                                          \
            grep -v -e ' [\._]*amudp_' -e ' [\._]*AMUDP_' -e ' [\._]*AM_'           \
                    -e ' [\._]*gasnett_' -e ' [\._]*gasneti_'                       \
                    -e 'get_pc_thunk'                                               \
                    -e __FUNCTION__ -e __PRETTY_FUNCTION__ -e debug_info_seg |      \
            @PERL@ -n -e 'print if /^[0-9a-fA-F]+\s+[A-Z]\s+/' > .$$lib.exp;        \
          if test -s .$$lib.exp ; then                                              \
            cat .$$lib.exp ;                                                        \
            echo FAILED ;                                                           \
            failed=1 ;                                                              \
          else                                                                      \
            echo PASSED ;                                                           \
          fi ;                                                                      \
          rm -f .$$lib.exp ;                                                        \
          exit $$failed
else
check-exports: libamudp.a
	@echo check-exports test SKIPPED
endif

# No check-exports by default due to name mangling
check: tests

clean_local_files = libamudp.a *.o core .test-results $(BUILT_SOURCES)
clean-local:
	@for f in $(testprograms); do                                \
	  cmd="rm -f '$$f$(EXESUFFIX)'"; echo "$$cmd"; eval "$$cmd" || exit $$? ; \
	 done;
else
clean_local_files =
clean-local:
	@: #empty rule
endif

CLEANFILES = $(clean_local_files)

force:

.PHONY: force tests run-tests

