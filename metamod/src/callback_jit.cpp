#include "precompiled.h"

CJit g_jit;

class CUniqueLabel : public std::string
{
public:
	CUniqueLabel(const char* name) : std::string(std::string(name) + std::to_string(m_unique_index++))
	{
	}

private:
	static size_t m_unique_index;
};

size_t CUniqueLabel::m_unique_index;

class CForwardCallbackJIT : public jitasm::function<void, CForwardCallbackJIT>
{
public:
	CForwardCallbackJIT(jitdata_t* jitdata);
	void naked_main();
	void call_func(Reg32 addr);
	void jit_debug(const char* format, ...);

private:
	jitdata_t* m_jitdata;

	enum
	{
		mg_mres = 0,
		mg_prev_mres = 4,
		mg_status = 8,
		mg_orig_ret = 12,
		mg_over_ret = 16,
		mg_esp_save = 20
	};

	enum
	{
		first_arg_offset = 12,
		xmmreg_size = 16
	};

	static size_t align(size_t v, size_t a)
	{
		return (v + a - 1) & ~(a - 1);
	}
};

CForwardCallbackJIT::CForwardCallbackJIT(jitdata_t* jitdata) : m_jitdata(jitdata)
{
}

void CForwardCallbackJIT::naked_main()
{
	// prologue
	push(ebx);
	push(ebp);
	mov(ebp, esp);
	and_(esp, 0xFFFFFFF0); // stack must be 16-aligned when we calling subroutines

	enum // stack map
	{
		/* META GLOBALS BACKUP */
		/* STRING BUFFER */
		over_ret = sizeof(int),
		orig_ret = 0
	};

	auto globals = ebx;
	auto locals_size = m_jitdata->rettype != rt_void ? sizeof(int) * 2 /* orig + over */ : 0;
	auto framesize = align(locals_size + sizeof(meta_globals_t) + /* for align */m_jitdata->args_count * sizeof(int), xmmreg_size) - m_jitdata->args_count * sizeof(int);

	if (m_jitdata->has_varargs) {
		size_t strbuf_offset = locals_size;

		sub(esp, framesize += align(MAX_STRBUF_LEN, xmmreg_size));

		// format varargs
		lea(edx, dword_ptr[ebp + first_arg_offset + m_jitdata->args_count * sizeof(int)]); // varargs ptr
		if (strbuf_offset)
			lea(eax, dword_ptr[esp + strbuf_offset]); // buf ptr
		else
			mov(eax, esp);
		mov(ecx, size_t(Q_vsnprintf));

		push(edx);
		push(dword_ptr[ebp + first_arg_offset + (m_jitdata->args_count - 1) * sizeof(int)]); // last arg of pfn (format string)
		push(MAX_STRBUF_LEN);
		push(eax);
		call(ecx);
		add(esp, 4 * sizeof(int));
	}
	else
		sub(esp, framesize);

	size_t mg_backup = framesize - sizeof(meta_globals_t);

	// setup globals ptr and backup old data
	mov(globals, size_t(&g_metaGlobals));
	movaps(xmm0, xmmword_ptr[globals]);
	movq(xmm1, qword_ptr[globals + xmmreg_size]);
	movaps(xmmword_ptr[esp + mg_backup + sizeof(int) * 2], xmm0);
	movq(qword_ptr[esp + mg_backup], xmm1);

	// call metamod's pre hook if present
	if (m_jitdata->mm_hook && m_jitdata->mm_hook_time == P_PRE) {
		mov(ecx, m_jitdata->mm_hook);
		call_func(ecx);
	}

	// setup meta globals
	mov(dword_ptr[globals + mg_mres], MRES_UNSET);
	mov(dword_ptr[globals + mg_status], MRES_UNSET);
	mov(dword_ptr[globals + mg_esp_save], esp);

	// setup retval pointers
	if (m_jitdata->rettype != rt_void) {
		lea(eax, dword_ptr[esp + over_ret]);
		mov(dword_ptr[globals + mg_orig_ret], esp);
		mov(dword_ptr[globals + mg_over_ret], eax);
	}

	// call pre
	for (auto plug : *m_jitdata->plugins) {
		if (plug->status() < PL_RUNNING) // allow only running and paused
			continue;

		size_t fn_table = *(size_t *)(size_t(plug) + m_jitdata->table_offset);

		// plugin don't want any hooks from that table
		if (!fn_table)
			continue;

		CUniqueLabel go_next_plugin("go_next_plugin");

		// check status and handler set
		mov(ecx, dword_ptr[fn_table + m_jitdata->pfn_offset]);
		cmp(byte_ptr[plug->status_ptr()], PL_RUNNING);
		jecxz(go_next_plugin);
		jnz(go_next_plugin);

		// update meta globals
		mov(eax, dword_ptr[globals + mg_mres]);
		mov(dword_ptr[globals + mg_mres], MRES_IGNORED);
		mov(dword_ptr[globals + mg_prev_mres], eax);

		call_func(ecx);

		// update highest meta result
		mov(edx, dword_ptr[globals + mg_mres]);
		mov(ecx, dword_ptr[globals + mg_status]);
		cmp(edx, ecx);
		cmovg(ecx, edx);
		mov(dword_ptr[globals + mg_status], ecx);

		// save return value if override or supercede
		if (m_jitdata->rettype != rt_void) {
			if (m_jitdata->rettype == rt_float) {
				sub(esp, sizeof(int));
				fstp(dword_ptr[esp]);
				pop(eax);
			}

			mov(ecx, dword_ptr[esp + over_ret]);
			cmp(edx, MRES_OVERRIDE);
			cmovae(ecx, eax);
			mov(dword_ptr[esp + over_ret], ecx);
		}

		L(go_next_plugin);
	}

	// call original if it needed
	cmp(dword_ptr[globals + mg_status], MRES_SUPERCEDE);
	jz("skip_original");
	{
		// and present
		if (m_jitdata->pfn_original) {
			mov(ecx, m_jitdata->pfn_original);
			call_func(ecx);
		}

		// store original return value
		if (m_jitdata->rettype == rt_integer) {
			if (m_jitdata->pfn_original)
				mov(dword_ptr[esp + orig_ret], eax);
			else
				mov(dword_ptr[esp + orig_ret], TRUE); // fix for should collide :/

			jmp("skip_supercede");
		}
		else if (m_jitdata->rettype == rt_float) {
			if (m_jitdata->pfn_original)
				fstp(dword_ptr[esp + orig_ret]);
			else
				mov(dword_ptr[esp + orig_ret], 0);

			jmp("skip_supercede");
		}
	}
	L("skip_original");
	{
		if (m_jitdata->rettype != rt_void) {
			// if supercede
			mov(eax, dword_ptr[esp + over_ret]);
			mov(dword_ptr[esp + orig_ret], eax);

			L("skip_supercede");
		}
	}
	L("skip_all");

	// call post
	for (auto plug : *m_jitdata->plugins) {
		if (plug->status() < PL_RUNNING) // allow only running and paused
			continue;

		size_t fn_table = *(size_t *)(size_t(plug) + m_jitdata->post_table_offset);

		// plugin don't want any hooks from that table
		if (!fn_table)
			continue;

		CUniqueLabel go_next_plugin("go_next_plugin");

		// check status and handler set
		mov(ecx, dword_ptr[fn_table + m_jitdata->pfn_offset]);
		cmp(byte_ptr[plug->status_ptr()], PL_RUNNING);
		jecxz(go_next_plugin);
		jnz(go_next_plugin);

		// update meta globals
		mov(eax, dword_ptr[globals + mg_mres]);
		mov(dword_ptr[globals + mg_mres], MRES_IGNORED);
		mov(dword_ptr[globals + mg_prev_mres], eax);

		call_func(ecx);

		// update highest meta result
		mov(edx, dword_ptr[globals + mg_mres]);
		mov(ecx, dword_ptr[globals + mg_status]);
		cmp(ecx, edx);
		cmovl(ecx, edx);
		mov(dword_ptr[globals + mg_status], ecx);

		// save return value if override or supercede
		if (m_jitdata->rettype != rt_void) {
			if (m_jitdata->rettype == rt_float) {
				sub(esp, sizeof(int));
				fstp(dword_ptr[esp]);
				pop(eax);
			}

			cmp(edx, MRES_OVERRIDE);
			mov(ecx, dword_ptr[esp + over_ret]);
			cmovae(ecx, eax);
			mov(dword_ptr[esp + over_ret], ecx);
		}

		L(go_next_plugin);
	}

	// call metamod's post hook if present
	if (m_jitdata->mm_hook && m_jitdata->mm_hook_time == P_POST) {
		mov(ecx, m_jitdata->mm_hook);
		call_func(ecx);
	}

	// setup return value and override it if needed
	if (m_jitdata->rettype == rt_integer) {
		mov(eax, dword_ptr[esp + orig_ret]);
		cmp(dword_ptr[globals + mg_status], MRES_OVERRIDE);
		cmovae(eax, dword_ptr[esp + over_ret]);
	}
	else if (m_jitdata->rettype == rt_float) {
		lea(eax, dword_ptr[esp + over_ret]);
		cmp(dword_ptr[globals + mg_status], MRES_OVERRIDE);
		cmovb(eax, esp); // orig_ret
		fld(dword_ptr[eax]);
	}

	// restore meta globals
	movaps(xmm0, xmmword_ptr[esp + mg_backup + sizeof(int) * 2]);
	movq(xmm1, qword_ptr[esp + mg_backup]);
	movaps(xmmword_ptr[globals], xmm0);
	movq(qword_ptr[globals + xmmreg_size], xmm1);

	// epilogue
	mov(esp, ebp);
	pop(ebp);
	pop(ebx);
	ret();
}

void CForwardCallbackJIT::call_func(Reg32 addr)
{
	const size_t fixed_args_count = m_jitdata->args_count - (m_jitdata->has_varargs ? 1u /* excluding format string */ : 0u);
	const size_t strbuf_offset = m_jitdata->rettype != rt_void ? sizeof(int) * 2u /* orig + over */ : 0u;

	// push formatted buf instead of format string
	if (m_jitdata->has_varargs) {
		if (strbuf_offset) {
			lea(eax, dword_ptr[esp + strbuf_offset]);
			push(eax);
		}
		else
			push(esp);

		push(size_t("%s"));
	}

	// push normal args
	for (size_t j = fixed_args_count; j > 0; j--)
		push(dword_ptr[ebp + first_arg_offset + (j - 1) * sizeof(int)]);

	// call
	call(addr);

	// pop stack
	if (m_jitdata->args_count)
		add(esp, (m_jitdata->args_count + (m_jitdata->has_varargs ? 1u : 0u)) * sizeof(int));
}

void CForwardCallbackJIT::jit_debug(const char* format, ...)
{
#ifdef JIT_DEBUG
	va_list argptr;
	char string[1024] = "";

	va_start(argptr, format);
	Q_vsnprintf(string, sizeof string, format, argptr);
	va_end(argptr);

	char* memory_leak = Q_strdup(string); // yes, I'm lazy
	static size_t print_ptr = size_t(&printf);

	pushad();
	push(size_t(memory_leak));
	call(dword_ptr[size_t(&print_ptr)]);
#ifdef JIT_DEBUG_FILE
	static size_t fprint_ptr = size_t(&mdebug_to_file);
	call(dword_ptr[size_t(&fprint_ptr)]);
#endif
	add(esp, 4);
	popad();
#endif
}

CJit::CJit() : m_callback_allocator(static_allocator::mp_rwx), m_tramp_allocator(static_allocator::mp_rwx)
{
}

size_t CJit::compile_callback(jitdata_t* jitdata)
{
	if (!is_hook_needed(jitdata)) {
		return jitdata->pfn_original;
	}

	CForwardCallbackJIT callback(jitdata);
	callback.Assemble();

	auto code = callback.GetCode();
	auto codeSize = callback.GetCodeSize();
	auto ptr = m_callback_allocator.allocate(codeSize);

	return (size_t)Q_memcpy(ptr, code, codeSize);
}

size_t CJit::compile_tramp(size_t ptr_to_func)
{
	auto code = (uint8 *)m_tramp_allocator.allocate(2 + sizeof(int));

	// jmp dword [ptr_to_func]
	code[0] = 0xFFu;
	code[1] = 0x25u;
	*(size_t *)&code[2] = ptr_to_func;

	return (size_t)code;
}

void CJit::clear_callbacks()
{
	m_callback_allocator.deallocate_all();
}

void CJit::clear_tramps()
{
	m_tramp_allocator.deallocate_all();
}

size_t CJit::is_callback_retaddr(uint32 addr)
{
	if (m_callback_allocator.contain(addr)) {
		// FF D1        call    ecx
		// 83 C4 20     add     esp, 20h ; optional
		// 8B 13        mov     edx, [ebx]
		char *ptr = (char *)addr - 2;
		return mem_compare(ptr, "\xFF\xD1\x83\xC4", 4) || mem_compare(ptr, "\xFF\xD1\x8B\x13", 4);
	}
	return false;
}

char* CJit::find_callback_pattern(char* pattern, size_t len)
{
	return m_callback_allocator.find_pattern(pattern, len);
}

bool CJit::is_hook_needed(jitdata_t* jitdata)
{
	if (!jitdata->pfn_original)
		return true;

	if (jitdata->mm_hook)
		return true;

	if (!jitdata->plugins)
		return false;

	for (auto& plug : *jitdata->plugins) {
		const size_t fn_table		= *(size_t *)(size_t(plug) + jitdata->table_offset);
		const size_t fn_table_post	= *(size_t *)(size_t(plug) + jitdata->post_table_offset);

		if (fn_table || fn_table_post) {
			return true;
		}
	}

	return false;
}
