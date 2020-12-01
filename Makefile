# Copyright 2020 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

CFLAGS = -O3 -Wall -Werror
LDFLAGS= -static -s -lpthread
DEPS   = util.h params.h mm.h lib.h Makefile

all: measure hammer

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

measure: lib.o mm.o measure.o params.o
	$(CC) -o $@ $^ $(LDFLAGS)

hammer: lib.o mm.o hammer.o params.o
	$(CC) -o $@ $^ $(LDFLAGS)

clean:
	rm *.o measure hammer
