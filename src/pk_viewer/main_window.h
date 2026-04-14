#pragma once

#include "netdevil/archive/pk/pk_reader.h"

#include <QMainWindow>
#include <QLabel>
#include <QTreeWidget>
#include <QLineEdit>

#include <unordered_map>
#include <memory>

namespace pk_viewer {

// One loaded PK archive.
struct LoadedPack {
    QString path;
    QString name;                   // filename only
    std::vector<uint8_t> data;
    std::unique_ptr<lu::assets::PkArchive> archive;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    bool openFile(const QString& path);
    void loadClientRoot(const QString& root);

private slots:
    void onFileOpen();
    void onSetClientRoot();
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

    // PKI data: pack file paths and CRC → pack index mapping
    std::vector<std::string> pkiPackPaths_;         // index → pack file path
    std::unordered_map<uint32_t, int> crcToPackIdx_; // CRC → pack index in pkiPackPaths_

    void loadPki();
};

} // namespace pk_viewer
