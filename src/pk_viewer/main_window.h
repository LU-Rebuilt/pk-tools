#pragma once

#include "netdevil/archive/pk/pk_reader.h"
#include "netdevil/archive/pki/pki_reader.h"
#include "netdevil/archive/sd0/sd0_reader.h"

#include <QMainWindow>
#include <QLabel>
#include <QTreeWidget>
#include <QLineEdit>

#include <unordered_map>
#include <memory>

namespace pk_viewer {

// Result of identifying one loose/decompressed file's format.
struct DetectedFile {
    QString type;   // short type tag shown in the tree ("NIF", "KFM", "FDB", ...)
    QString name;   // best-effort display name recovered from the file's own content,
                     // or empty if the format carries none / it couldn't be determined
};

// One loaded PK archive, OR (when archive is null) one loose file decompressed from a
// standalone .sd0 on disk — a "pack" of exactly one entry, so every entry-indexed code
// path (extraction, type detection, search) works unchanged for both. loose_data holds
// the decompressed bytes in the latter case; loose_original_name is the .sd0's filename
// with the ".sd0" suffix stripped (its manifest-relative path is generally unknown, so
// resolveFilename can't help here the way it does for packed entries).
struct LoadedPack {
    QString path;
    QString name;                   // filename only
    std::vector<uint8_t> data;
    std::unique_ptr<lu::assets::PkArchive> archive;

    std::vector<uint8_t> loose_data;
    QString loose_original_name;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    bool openFile(const QString& path);
    void loadClientRoot(const QString& root);
    void loadSd0Folder(const QString& dir);

private slots:
    void onFileOpen();
    void onSetClientRoot();
    void onOpenSd0Folder();
    void onExtractSelected();
    void onExtractAll();
    void onSearchChanged(const QString& text);
    void onItemDoubleClicked(QTreeWidgetItem* item, int column);
    void onContextMenu(const QPoint& pos);

private:
    void buildTree();
    void loadManifest();
    void loadAllPacks();
    QString resolveFilename(uint32_t crc) const;
    QString detectFileType(const std::vector<uint8_t>& data) const;
    DetectedFile detectFile(const std::vector<uint8_t>& data) const;
    QString formatSize(uint32_t bytes) const;
    void extractEntry(int packIdx, int entryIdx, const QString& destPath);

    QTreeWidget* tree_ = nullptr;
    QLineEdit* searchBox_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    // All loaded PK archives
    std::vector<LoadedPack> packs_;

    // Client root for manifest resolution
    QString clientRoot_;
    bool hasVersions_ = false;

    // CRC → filename mapping (from trunk.txt)
    std::unordered_map<uint32_t, std::string> crcMap_;

    // PKI data (pack file paths + CRC → pack index), parsed via the shared pki_reader.
    lu::assets::PkiFile pki_;

    void loadPki();
};

} // namespace pk_viewer
