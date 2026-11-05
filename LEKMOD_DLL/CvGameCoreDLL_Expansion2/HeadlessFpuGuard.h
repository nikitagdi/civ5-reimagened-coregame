#pragma once
// FIX-0017 / DS-0028 (flag LEKMOD_HEADLESS_FIX_0017): FPU_PRECISION_GUARD.
//
// The gamecore is compiled /fp:strict for the x87 (no SSE2): its float results, and through them AI decisions
// (ReweightByTurnsLeft, IsYieldDeficient, ...), depend on the precision-control field of the x87 control word of the
// thread that calls into the DLL. 53 bit (the 0x027F default of a 32-bit Windows process) and 64 bit give the same
// simulation (the DLL stores every intermediate result to a double); 24 bit does not (lekmod6 diverges at turn 42-44,
// Direct3D 9 without D3DCREATE_FPU_PRESERVE switches the creating
// thread to 24 bit, Direct3D 11 does not touch it, so a DX9 peer whose game-core thread is that thread would simulate
// differently from a DX11 peer in the same lobby.
//
// CvHeadlessFpuGuard pins precision control to 53 bit and rounding control to nearest for the duration of one entry into
// the DLL and puts the two fields back on exit (the caller's own float code, e.g. D3D9, keeps the mode it set up).
//  - Only bits 8-11 of the x87 control word (precision, rounding) are read and written, with fnstcw / fldcw. The
//    exception masks, the infinity bit and the SSE control/status register (MXCSR) are never touched. (_controlfp_s with
//    _MCW_RC would also write the SSE2 rounding bits of MXCSR on MSVCR90, measured; hence the plain x87 instructions.)
//  - With the control word the real game normally has (0x027F) the guard is one fnstcw and a compare per entry: no write,
//    nothing to restore, simulation identical to a build without the guard.
//  - Guards nest (an inner guard sees 53 bit and does nothing) and are per thread (the control word is).
//  - Diag builds report each entry where the guard had to change the mode as OnFixTrigger(17).
// Use: HEADLESS_FPU_GUARD(); as the first statement of a function the exe calls into the DLL for (simulation or state
// loading), always fenced by #if defined(LEKMOD_HEADLESS_FIX_0017): the golden build does not include this header at all
// (_Defines.h pulls in HeadlessImprovements.h only for LEKMOD_HEADLESS_BUILD), so an unfenced use would not compile there.
// Without the flag the DLL is byte-for-byte golden.
#if defined(LEKMOD_HEADLESS_FIX_0017)

#define HEADLESS_X87_PCRC_MASK		0x0F00	// bits 8-9 precision control, bits 10-11 rounding control
#define HEADLESS_X87_PC53_RCNEAR	0x0200	// precision 53 bit (10b), round to nearest (00b)

class CvHeadlessFpuGuard
{
public:
	CvHeadlessFpuGuard()
		: m_usSavedPcRc(0)
		, m_bChanged(false)
	{
		unsigned short usControlWord = 0;
		__asm fnstcw usControlWord
		if((usControlWord & HEADLESS_X87_PCRC_MASK) != HEADLESS_X87_PC53_RCNEAR)
		{
			m_usSavedPcRc = (unsigned short)(usControlWord & HEADLESS_X87_PCRC_MASK);
			m_bChanged = true;
			unsigned short usNewControlWord = (unsigned short)((usControlWord & ~HEADLESS_X87_PCRC_MASK) | HEADLESS_X87_PC53_RCNEAR);
			__asm fldcw usNewControlWord
			HEADLESS_FIX_TRIGGER(17);
		}
	}
	~CvHeadlessFpuGuard()
	{
		if(m_bChanged)
		{
			unsigned short usControlWord = 0;
			__asm fnstcw usControlWord
			usControlWord = (unsigned short)((usControlWord & ~HEADLESS_X87_PCRC_MASK) | m_usSavedPcRc);
			__asm fldcw usControlWord
		}
	}

private:
	CvHeadlessFpuGuard(const CvHeadlessFpuGuard&);
	CvHeadlessFpuGuard& operator=(const CvHeadlessFpuGuard&);

	unsigned short m_usSavedPcRc;
	bool m_bChanged;
};

#define HEADLESS_FPU_GUARD()	CvHeadlessFpuGuard kHeadlessFpuGuard

#endif
