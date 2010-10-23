
#include <string.h>

#include "jit_code.h"
#include "jit_fragment.h"
#include "scratch.h"
#include "runtime.h"
#include "error.h"
#include "syscalls.h"
#include "mm.h"
#include "debug.h"

/* On the event of a signal we may need to finish the emulation of an instruction.
 * So we need to translate our jit code of one emulated instruction into code which
 * switches back after that instruction is done.
 */

static long jit_fragment_jump_relative(char *dest, instr_t *instr, char *jump_jit_addr)
{
	return gen_code(
		dest,

		"0F 80+ L",                             /* jcc rel32                        */

		instr->addr[instr->mrm-1] & 0x0f,
		jump_jit_addr-&dest[6]
	);
}

static long jit_fragment_jump_relative_exit(char *dest, instr_t *instr, char *jump_addr)
{
	return gen_code(
		dest,

		"70+ 10"                                /* jcc end (inversed)               */
		"C7 05 L L"                             /* movl $addr, jit_eip              */
		"FF 25 L",                              /* jmp *jit_fragment_exit_addr      */

		(instr->addr[instr->mrm-1]^1) & 0x0f,
		&jit_eip, jump_addr,
		&jit_fragment_exit_addr
	);
}

static long jit_fragment_jump(char *dest, char *jump_jit_addr)
{
	return gen_code(
		dest,

		"E9 L",                                 /* jmp rel32                         */

		jump_jit_addr-&dest[5]
	);
}

static long jit_fragment_jump_exit(char *dest, char *jump_addr)
{
	return gen_code(
		dest,

		"C7 05 L L"                             /* movl $addr, jit_eip               */
		"FF 25 L",                              /* jmp *jit_fragment_exit_addr       */

		&jit_eip, jump_addr,
		&jit_fragment_exit_addr
	);
}

static long jit_fragment_jump_indirect_exit(char *dest, instr_t *instr, char *jump_addr)
{
	long code_len, i;
	code_len = gen_code(
		dest,

		"A3 L"                                  /* movl %eax, jit_fragment_scratch     */
		"? 8B &$"                               /* movl (seg:)?mem, %eax               */
		"A3 L"                                  /* movl %eax, jit_eip                  */
		"A1 L"                                  /* movl jit_fragment_scratch, %eax     */
		"FF 25 L",                              /* *jit_fragment_exit_addr             */

		&jit_fragment_scratch,    
		instr->p2, &i, &instr->addr[instr->mrm], instr->len-instr->mrm,
		&jit_eip,
		&jit_fragment_scratch,    
		&jit_fragment_exit_addr
	);
	dest[i] &= 0xC7;
	return code_len;
}

static long jit_fragment_control(char *dest, instr_t *instr,
                              char *addr, unsigned long len,
                              char *mapping[])
{
	long code_len, off=0;
	char *pc = &instr->addr[instr->len],
	     *jump_addr = pc + imm_at(&instr->addr[instr->imm], instr->len-instr->imm);

	switch (instr->action)
	{
		case JUMP_CONDITIONAL:
			if ( contains(addr, len+1, jump_addr) )
				return jit_fragment_jump_relative(dest, instr, mapping[jump_addr-addr]);
			else
				return jit_fragment_jump_relative_exit(dest, instr, jump_addr);

		case LOOP: /* loops */
			off = gen_code(
				dest,

				"$  02"
				"EB FF",

				instr->addr, instr->len-1
			);

			if ( contains(addr, len+1, jump_addr) )
				code_len = off + jit_fragment_jump(&dest[off], mapping[jump_addr-addr]);
			else
				code_len = off + jit_fragment_jump_exit(&dest[off], jump_addr);

			dest[off-1] = code_len-off;

			return code_len;

		case JUMP_RELATIVE:
			if ( contains(addr, len+1, jump_addr) )
				return jit_fragment_jump(dest, mapping[jump_addr-addr]);
			else
				return jit_fragment_jump_exit(dest, jump_addr);

		case JUMP_INDIRECT:
			if ( contains(runtime_code_start, RUNTIME_CODE_SIZE, *(char **)jump_addr) )
			{
				memcpy(dest, instr->addr, instr->len);
				return instr->len; /* jumps to runtime code are caught when jumping back */
			}
			else
				return jit_fragment_jump_indirect_exit(dest, instr, jump_addr);
		case CALL_RELATIVE: /* unimplemented, not needed, (not used by code generation) */
		case CALL_INDIRECT:
		case JUMP_FAR:
		case JOIN:
		case RETURN:
		case RETURN_CLEANUP:
		default:
			break;
	}
	die("unexpected translated code: %d", instr->action);
	return -1;
}

static char *jit_fragment_translate(char *addr, long len, char *entry,
                                    char jit_addr[], long jit_len, char *mapping[])
{
	instr_t instr;

	long s_off=0, d_off=0;
	char *jit_entry = NULL;

	for ( ;; )
	{
		mapping[s_off] = &jit_addr[d_off];

		if ( s_off > len )
			die("instuction sticks out of fragment");

		if ( s_off == len )
		{
			jit_fragment_jump_exit(&jit_addr[d_off], &addr[s_off]);
			break;
		}

		if ( &addr[s_off] == entry )
			jit_entry = &jit_addr[d_off];

		if ( read_op(&addr[s_off], &instr, len-s_off) == CODE_JOIN )
			die("jit_fragment too large");

		if ( (instr.action & CONTROL_MASK) == CONTROL )
			d_off += jit_fragment_control(&jit_addr[d_off], &instr, addr, len, mapping);
		else
		{
			memcpy(&jit_addr[d_off], instr.addr, instr.len);
			d_off += instr.len;
		}

		s_off += instr.len;
	}

	if ( jit_entry == NULL )
		die("fragment entry point not in translated code");

	return jit_entry;
}

char *jit_fragment(char *fragment, long len, char *entry)
{
	char *jit_entry;
	char *mapping[len+1];

	sys_mprotect(jit_fragment_page, PG_SIZE, PROT_READ|PROT_WRITE);

	/* two-pass, we build up the jump-mapping beforehand */
	            jit_fragment_translate(fragment, len, entry, jit_fragment_page, PG_SIZE, mapping);
	jit_entry = jit_fragment_translate(fragment, len, entry, jit_fragment_page, PG_SIZE, mapping);

	sys_mprotect(jit_fragment_page, PG_SIZE, PROT_EXEC);

	return jit_entry;
}
