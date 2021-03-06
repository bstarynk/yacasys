## file Makefile
.PHONY: all clean modules indent
CC=gcc
OPTIMFLAGS= -g -O
CFLAGS= -std=gnu99 -Wall -pthread -I /usr/local/include/ $(OPTIMFLAGS)
LIBES= -L /usr/local/lib -ljansson -lfcgi -lrt -lm  -lcrypt -ldl
CSOURCES= $(wildcard src/[a-z]*.c)
MODSOURCES= $(wildcard src/[1-9_][^_]*.c)
COBJECTS= $(patsubst src/%.c, obj/%.o, $(CSOURCES))
MODULES= $(patsubstr src/%.c, obj/%.so, $(MODSOURCES))
RM= rm -vf
INDENT= indent -gnu
all: yacasys.fcgi
	ls -l $^


modules: $(MODULES)

.SUFFIXES: .so

clean:
	$(RM) obj/*.o src/*~ src/*orig src/*bak obj/*so yacasys.fcgi *log __*.c __*.o

yacasys.fcgi: $(COBJECTS)  __buildstamp__.c
	$(LINK.c) -rdynamic $^ -o $@-tmp $(LIBES) && mv -f $@-tmp $@
	rm __buildstamp__.c

__buildstamp__.c:
	date +'const char yaca_build_timestamp[]="%Y %b %d %H:%M:%S %Z";' > $@

obj/%.so: src/%.c src/yaca.h
	$(COMPILE.c) -fPIC -shared $< -o $@ 

obj/%.o: src/%.c src/yaca.h
	$(COMPILE.c) $< -o $@

$(COBJECTS): src/yaca.h

indent:
	for f in src/yaca.h $(CSOURCES) $(MODSOURCES) ; do $(INDENT) $$f; done
