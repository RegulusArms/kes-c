// Archive passwords in the desktop's keyring (the Secret Service: GNOME Keyring, KWallet), through libsecret.
//
// Saved only when the user ticks "Remember this password", in the login keyring as Kestrel's own entries (schema
// org.kestrel_explorer.ArchivePassword, labelled "Kestrel Explorer: archive password for photos.7z"): any app running
// as the user can read that keyring, so nothing is saved without asking. An archive is recognised by its fingerprint
// (its size and a hash of its start), so renaming or moving it still finds its password. No keyring, or one that
// doesn't speak the Secret Service: nothing is saved or found, and everything else works as before.
//
// Every call talks to the keyring over D-Bus (and it may ask to be unlocked): on a worker thread only, never the UI's.
#pragma once

#include <QString>
#include <QStringList>

namespace keyring {

QString fingerprint(const QString &archive);   // its size and the SHA-256 of its first 64 KB ("" if unreadable)
bool available();                              // a Secret Service answers
// saved for this archive, or empty; label_name: the archive's name, for the entry's label
QString lookup(const QString &fingerprint);
QStringList all();                             // every saved archive password (each once)
bool store(const QString &fingerprint, const QString &name, const QString &password);
int forget_all();                              // how many were forgotten; -1 if the keyring couldn't be reached

}  // namespace keyring
