// Window diagnostics (developer tools: ☰ → Tower… → Windows): every window of this Kestrel as Qt sees it (its size and
// position, the minimum and maximum it asks the desktop for, its state, its screen, the border's margins) and its
// recent size, position and state changes, to compare a window that misbehaves (one that can't be resized, say) with a
// fresh one. Only this process's windows: each Kestrel has its own (open the Tower from the window to look at).
#pragma once

#include <QString>

namespace windiag {

void start();       // record from now on (at startup)
QString report();   // every window of this Kestrel, and what was recorded, as text

}  // namespace windiag
