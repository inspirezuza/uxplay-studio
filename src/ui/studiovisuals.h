#pragma once

#include <QIcon>
#include <QPixmap>
#include <QString>

namespace StudioVisuals {
QIcon icon(const QString &name);
QPixmap projectThumbnail(const QString &title, bool recoverable);
}
