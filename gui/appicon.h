#pragma once

#include <QIcon>

QIcon createAppIcon(int unreadCount = 0);
// A green (online) or red (offline) dot, e.g. for a conversation tab.
QIcon createStatusIcon(bool online);
