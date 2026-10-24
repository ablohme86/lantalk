#pragma once

#include <QIcon>

QIcon createAppIcon(int unreadCount = 0);
enum class Presence { Online, Away, Offline };
// A green, yellow, or red dot, e.g. for a conversation tab.
QColor presenceColor(Presence presence);
QIcon createStatusIcon(Presence presence);
