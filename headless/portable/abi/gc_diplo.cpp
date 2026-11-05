// Q_DIPLO (task N3.10; record gcq_diplo in gamecore_abi.h). One record per other living major civ: met, at war, human, and whether the
// commands DECLARE_WAR / MAKE_PEACE would be accepted for the player's team now (the same checks the commands make: CheckDeclareWar, CheckMakePeace in
// gc_command.cpp), plus the turns the war is still locked. Nothing here changes the game (AC-N1.4): the checks only read the core.
#include "CvGameCoreDLLPCH.h"

#include "gc_command.h"
#include "gc_diplo.h"
#include "gc_gcq_writer.h"
#include "../runtime/rt_active.h"

#include <string.h>

namespace lekmod_portable {

namespace {
#define GCQ_ASSERT_SIZE(type, size) typedef char gcq_size_check_##type[(sizeof(type) == (size)) ? 1 : -1]
GCQ_ASSERT_SIZE(gcq_diplo, 8);
}  // namespace

int32_t QueryDiplo(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg)
{
	if (argc != 1)
	{
		msg = "Q_DIPLO takes one argument: player";
		return GC_ERR_INVALID_ARG;
	}
	const int me = args[0];
	if (me < 0 || me >= MAX_MAJOR_CIVS || !GET_PLAYER((PlayerTypes)me).isAlive() || GET_PLAYER((PlayerTypes)me).isObserver())
	{
		msg = "Q_DIPLO: player is not a living major civilization";
		return GC_ERR_INVALID_ARG;
	}
	CvPlayer& self = GET_PLAYER((PlayerTypes)me);
	const TeamTypes myTeam = self.getTeam();
	GcqBuf w(GC_Q_DIPLO, sizeof(gcq_diplo));
	for (int i = 0; i < MAX_MAJOR_CIVS; ++i)
	{
		if (i == me) continue;
		const CvPlayer& other = GET_PLAYER((PlayerTypes)i);
		if (!other.isAlive() || other.isObserver()) continue;
		gcq_diplo r;
		memset(&r, 0, sizeof(r));
		r.other = (int8_t)i;
		r.team = (int8_t)other.getTeam();
		unsigned flags = 0;
		if (GET_TEAM(myTeam).isHasMet(other.getTeam())) flags |= GC_DIPLO_F_MET;
		if (GET_TEAM(myTeam).isAtWar(other.getTeam())) flags |= GC_DIPLO_F_AT_WAR;
		if (IsSeatedHuman(i)) flags |= GC_DIPLO_F_HUMAN;
		std::string why;
		int32_t reason = 0;
		if (CheckDeclareWar(self, (int)other.getTeam(), why, &reason)) flags |= GC_DIPLO_F_CAN_DECLARE_WAR;
		if (CheckMakePeace(self, (int)other.getTeam(), NULL, why, &reason)) flags |= GC_DIPLO_F_CAN_MAKE_PEACE;
		r.flags = (uint16_t)flags;
		const int locked = WarLockedTurns((int)myTeam, (int)other.getTeam());
		r.war_locked_turns = (int16_t)(locked > 32767 ? 32767 : locked);
		w.Add(&r);
	}
	return w.Finish(out) ? GC_OK : (msg = "out of memory", GC_ERR_INTERNAL);
}

}  // namespace lekmod_portable
