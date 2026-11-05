// Q_NOTIFICATIONS (task F7; v2: task N3.7). Single game type: the runtime's registry (RtUserInterface: kept from
// AddNotification until RemoveNotification, strings localized by the gamecore). Network game: the live CvNotifications of the player, read from the core
// (RtUserInterface::ReadCoreNotifications), so every human has his own list; a player without a list (an AI, the server seat) has no records.
// C++03. The GCQ1 buffer is the shared GcqBuf (gc_gcq_writer.h).
#include "CvGameCoreDLLPCH.h"

#include "gc_notify.h"

#include <map>
#include <string.h>
#include <string>
#include <vector>

#include "../runtime/rt_ui.h"
#include "gc_gcq_writer.h"

namespace lekmod_portable {

namespace {

typedef char gcq_notification_size_check[(sizeof(gcq_notification) == 32) ? 1 : -1];

}  // namespace

int32_t QueryNotifications(const int32_t* args, uint32_t argc, gc_buf* out, std::string& msg)
{
	if (argc != 1)
	{
		msg = "Q_NOTIFICATIONS takes one argument: player";
		return GC_ERR_INVALID_ARG;
	}
	if (args[0] < 0 || args[0] >= MAX_PLAYERS)
	{
		msg = "Q_NOTIFICATIONS: player out of range";
		return GC_ERR_INVALID_ARG;
	}
	// The UI the session installed (RtSession::Load: SetEngineUserInterface(&m_ui)) is the only ICvUserInterface2 implementation in the build.
	const RtUserInterface* ui = static_cast<const RtUserInterface*>(GC.GetEngineUserInterface());
	GcqBuf buf(GC_Q_NOTIFICATIONS, (uint32_t)sizeof(gcq_notification));
	if (ui)
	{
		std::vector<RtNotification> list;
		ui->NotificationsOf((PlayerTypes)args[0], list);
		for (size_t i = 0; i < list.size(); ++i)
		{
			const RtNotification& n = list[i];
			gcq_notification r;
			memset(&r, 0, sizeof(r));
			r.id = n.id;
			r.type = (uint32_t)n.type;
			r.x = (int16_t)n.x;
			r.y = (int16_t)n.y;
			r.data1 = n.gameData;
			r.data2 = n.extraGameData;
			r.turn = (int16_t)n.turn;
			r.dismissed = 0;  // a dismissed notification is removed (RemoveNotification), so every listed one is live
			r.message = buf.Str(n.message.c_str());
			r.summary = buf.Str(n.summary.c_str());
			buf.Add(&r);
		}
	}
	if (!buf.Finish(out))
	{
		msg = "out of memory";
		return GC_ERR_INTERNAL;
	}
	return GC_OK;
}

}  // namespace lekmod_portable
