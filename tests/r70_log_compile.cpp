// Host-only syntax test of the two new R70 Xbox diagnostic log statements.
#include "native/runtime.h"
extern "C" void Xbox_Log(const char*, ...);
int main(){ZamnRuntimeWindowStats st = {};
#if defined(ZAMN_R70_HOT_THREAD_9A6D) && defined(ZAMN_R39_BUFFERED_LOG)
  Xbox_Log("R70THREAD hot-9A6D-approved=%llu saved-copy-bytes=%llu\n",
    (unsigned long long)st.r70_thread_9a6d_approved,
    (unsigned long long)(st.r70_thread_9a6d_approved * 131072ULL));
#endif
#if defined(ZAMN_R70_PAUSE_TRACE) && defined(ZAMN_R39_BUFFERED_LOG) && !defined(ZAMN_RELEASE_NO_DIAGNOSTICS)
  Xbox_Log("R70PAUSE release1=%llu press=%llu release2=%llu start-down=%llu/%llu/%llu\n",
    (unsigned long long)st.r70_pause_polls[0],
    (unsigned long long)st.r70_pause_polls[1],
    (unsigned long long)st.r70_pause_polls[2],
    (unsigned long long)st.r70_pause_start_down[0],
    (unsigned long long)st.r70_pause_start_down[1],
    (unsigned long long)st.r70_pause_start_down[2]);
#endif
return 0;}
