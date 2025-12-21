#pragma once

// Installs uploaded asset packs (zips, see shared/models/pack_archive.h), one at a time on their own thread: check,
// extract into data/textures/.incoming, move into place, scan, add to the catalogue. An existing pack (same name and
// type) is never replaced; the upload and the staging folder are removed whatever the outcome.

#include <QFuture>
#include <QString>

#include "mutations.h"

namespace PackInstall {

// data/textures/.incoming: uploads and extraction (same file system as the packs, so placing is a rename).
QString incomingDir();
// Removes what a previous run left in incomingDir() (startup).
void cleanIncoming();

// Before an upload of `bytes`: is a pack with this name and type installed, is there room?
MutationResult precheck(const QString &name, const QString &type, qint64 bytes);
// Room for an upload of `bytes` (the zip, plus as much again extracted).
bool hasRoomFor(qint64 bytes);

QFuture<MutationResult> install(const QString &zipPath, const ClientRef &by);

} // namespace PackInstall
