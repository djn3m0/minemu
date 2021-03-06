
/* This file is part of minemu
 *
 * Copyright 2010-2011 Erik Bosman <erik@minemu.org>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef JMP_CACHE_H
#define JMP_CACHE_H

#include "threads.h"

void add_jmp_mapping(char *addr, char *jit_addr);
char *find_jmp_mapping(char *addr);
void clear_jmp_cache(thread_ctx_t *ctx, char *addr, unsigned long len);

#define HASH_INDEX(addr) ((unsigned long)(addr)&0xfffful)

/* ( address + CACHE_MANGLE(addr) - 1 ) == 0
 * so that we can use lea+jecxz to check for equivalence,
 * leaving the CPU flags as they are.
 */
#define CACHE_MANGLE(addr) ((char*)(1-(unsigned long)(addr)))

#endif /* JMP_CACHE_H */
