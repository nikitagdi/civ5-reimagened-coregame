// Standalone headless event queue regression; no game data or gamecore archive needed.
#include "../../portable/runtime/rt_events.h"
#include <assert.h>

int main()
{
	using namespace lekmod_portable;
	RtEventQueue queue;
	RtEvent capture = RtEventQueue::Make(GC_EVENT_POPUP_INFO, 10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
	capture.hasText = true;
	capture.text = "capture fixture";
	queue.Push(capture);
	capture.text.clear();
	capture.args[9] = -1;
	queue.Push(RtEventQueue::Make(GC_EVENT_POPUP_INFO, 8, 11, 12, 13, 14, 15, 16, 17, 18));
	RtEvent result;
	assert(queue.Pop(result));
	assert(result.argc == 10 && result.hasText && result.text == "capture fixture");
	for (int i = 0; i < 10; ++i) assert(result.args[i] == i + 1);
	assert(queue.Pop(result));
	assert(result.argc == 8 && !result.hasText && !result.hasText2);
	for (int i = 0; i < 8; ++i) assert(result.args[i] == i + 11);
	assert(result.args[8] == 0 && result.args[9] == 0);
	assert(!queue.Pop(result));
	return 0;
}
