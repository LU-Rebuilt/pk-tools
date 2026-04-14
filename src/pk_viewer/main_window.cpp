#include "main_window.h"
#include "file_browser.h"

#include <QMenuBar>
#include <QStatusBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QSettings>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QApplication>
#include <QProgressDialog>

#include <QtConcurrent>
#include <QFutureWatcher>
#include <QTemporaryDir>
#include <QCoreApplication>

#include <algorithm>
#include <mutex>

namespace pk_viewer {

namespace {

// CRC-32/MPEG-2 matching DarkflameServer AssetManager::crc32b().
// Polynomial 0x04C11DB7, init 0xFFFFFFFF, no final XOR.
uint32_t crc32_mpeg2(uint32_t crc, const void* data, size_t len) {
    auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(bytes[i]) << 24;
        for (int j = 0; j < 8; ++j) {
            uint32_t msb = crc >> 31;
            crc <<= 1;
            if (msb) crc ^= 0x04C11DB7;
        }
    }
    return crc;
}

// Compute the pack CRC for a normalized path (lowercase, backslashes).
// Matches DarkflameServer: crc32b(crc32b(0xFFFFFFFF, path), "\0\0\0\0")
uint32_t computePackCrc(const std::string& path) {
    static const uint8_t null_term[4] = {0, 0, 0, 0};
    uint32_t crc = crc32_mpeg2(0xFFFFFFFF, path.data(), path.size());
    crc = crc32_mpeg2(crc, null_term, 4);
    return crc;
}

} // anonymous namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("PK Archive Viewer");
    resize(1000, 650);

    // Central widget with search + tree
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);

    searchBox_ = new QLineEdit;
    searchBox_->setPlaceholderText("Filter files...");
    searchBox_->setClearButtonEnabled(true);
    layout->addWidget(searchBox_);

    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({"Filename / CRC", "Size", "Compressed", "Type", "Offset"});
    tree_->setAlternatingRowColors(true);
    tree_->setSortingEnabled(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
    tree_->header()->setStretchLastSection(true);
    tree_->setColumnWidth(0, 400);
    tree_->setColumnWidth(1, 90);
    tree_->setColumnWidth(2, 90);
    tree_->setColumnWidth(3, 80);
    tree_->setColumnWidth(4, 90);
    layout->addWidget(tree_);

    setCentralWidget(central);

    // Menu bar
    auto* fileMenu = menuBar()->addMenu("&File");
    fileMenu->addAction("&Open...", QKeySequence::Open,
                        this, &MainWindow::onFileOpen);
    fileMenu->addSeparator();
    fileMenu->addAction("Set &Client Root...", QKeySequence("Ctrl+R"),
                        this, &MainWindow::onSetClientRoot);
    fileMenu->addSeparator();
    fileMenu->addAction("Extract &Selected...", QKeySequence("Ctrl+E"),
                        this, &MainWindow::onExtractSelected);
    fileMenu->addAction("Extract &All...", QKeySequence("Ctrl+Shift+E"),
                        this, &MainWindow::onExtractAll);
    fileMenu->addSeparator();
    fileMenu->addAction("&Quit", QKeySequence::Quit,
                        this, &QMainWindow::close);

    // Status bar
    statusLabel_ = new QLabel("No archive loaded");
    statusBar()->addPermanentWidget(statusLabel_);

    // Connections
    connect(searchBox_, &QLineEdit::textChanged, this, &MainWindow::onSearchChanged);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, &MainWindow::onItemDoubleClicked);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &MainWindow::onContextMenu);

    // Restore settings
    QSettings settings;
    QString savedRoot = settings.value("pk_client_root").toString();
    if (!savedRoot.isEmpty() && QDir(savedRoot).exists()) {
        loadClientRoot(savedRoot);
    }
}

bool MainWindow::openFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, "Error",
            QString("Could not open file:\n%1").arg(path));
        return false;
    }

    LoadedPack pack;
    pack.path = path;
    pack.name = QFileInfo(path).fileName();
    QByteArray raw = file.readAll();
    pack.data.assign(raw.begin(), raw.end());

    try {
        pack.archive = std::make_unique<lu::assets::PkArchive>(
            std::span<const uint8_t>(pack.data.data(), pack.data.size()));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Error",
            QString("Failed to parse PK archive:\n%1\n%2").arg(path, e.what()));
        return false;
    }

    // Single file open — show only this pack
    packs_.clear();
    packs_.push_back(std::move(pack));

    // Load manifest if we have a client root
    if (crcMap_.empty() && !clientRoot_.isEmpty()) {
        loadManifest();
        loadPki();
    }

    setWindowTitle(QString("PK Viewer - %1").arg(QFileInfo(path).fileName()));

    uint32_t entries = static_cast<uint32_t>(packs_[0].archive->entry_count());
    statusLabel_->setText(
        QString("Entries: %1 | Resolved: %2")
            .arg(entries).arg(crcMap_.size()));

    buildTree();
    return true;
}

void MainWindow::loadClientRoot(const QString& root) {
    clientRoot_ = root;

    // Check for versions directory
    hasVersions_ = QDir(root + "/versions").exists() ||
                   QDir(root + "/client/versions").exists();

    loadManifest();
    loadPki();

    if (hasVersions_) {
        loadAllPacks();
    }

    buildTree();

    uint32_t totalEntries = 0;
    for (const auto& p : packs_)
        totalEntries += static_cast<uint32_t>(p.archive->entry_count());

    statusLabel_->setText(
        QString("Packs: %1 | Entries: %2 | Resolved: %3")
            .arg(packs_.size())
            .arg(totalEntries)
            .arg(crcMap_.size()));

    if (packs_.size() == 1) {
        setWindowTitle(QString("PK Viewer - %1").arg(packs_[0].name));
    } else if (!packs_.empty()) {
        setWindowTitle(QString("PK Viewer - %1 packs").arg(packs_.size()));
    }
}

void MainWindow::loadAllPacks() {
    // Find all .pk files under the client root
    QStringList packDirs = {
        clientRoot_ + "/client/res/pack",
        clientRoot_ + "/res/pack",
    };

    QStringList pkFiles;
    for (const auto& dir : packDirs) {
        QDirIterator it(dir, {"*.pk"}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            pkFiles << it.filePath();
        }
        if (!pkFiles.isEmpty()) break;
    }

    if (pkFiles.isEmpty()) return;

    pkFiles.sort();
    packs_.clear();

    QProgressDialog progress("Loading PK archives...", "Cancel", 0,
                              static_cast<int>(pkFiles.size()), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);

    // Load and parse all packs in parallel
    std::mutex resultMutex;
    std::vector<LoadedPack> results;
    std::atomic<int> completed{0};

    QFutureWatcher<void> watcher;
    connect(&watcher, &QFutureWatcher<void>::progressValueChanged,
            &progress, &QProgressDialog::setValue);

    auto future = QtConcurrent::map(pkFiles, [&](const QString& pkPath) {
        QFile file(pkPath);
        if (!file.open(QIODevice::ReadOnly)) return;

        LoadedPack pack;
        pack.path = pkPath;
        pack.name = QFileInfo(pkPath).fileName();
        QByteArray raw = file.readAll();
        pack.data.assign(raw.begin(), raw.end());

        try {
            pack.archive = std::make_unique<lu::assets::PkArchive>(
                std::span<const uint8_t>(pack.data.data(), pack.data.size()));
        } catch (...) {
            return;
        }

        std::lock_guard lock(resultMutex);
        results.push_back(std::move(pack));
        int done = ++completed;
        // Update progress from main thread via signal
        QMetaObject::invokeMethod(&progress, [&progress, done, &pkFiles]() {
            progress.setValue(done);
            progress.setLabelText(
                QString("Loaded %1 / %2 packs").arg(done).arg(pkFiles.size()));
        }, Qt::QueuedConnection);
    });

    watcher.setFuture(future);

    // Process events while waiting so the progress dialog stays responsive
    while (!future.isFinished()) {
        QApplication::processEvents();
        if (progress.wasCanceled()) {
            future.cancel();
            break;
        }
        QThread::msleep(50);
    }
    future.waitForFinished();

    // Sort by name for consistent display order
    std::sort(results.begin(), results.end(),
              [](const LoadedPack& a, const LoadedPack& b) { return a.name < b.name; });

    packs_ = std::move(results);
    progress.setValue(static_cast<int>(pkFiles.size()));
}

void MainWindow::onFileOpen() {
    QSettings settings;
    QString lastDir = settings.value("pk_last_open_dir").toString();

    QString path = qt_common::FileBrowserDialog::getOpenFileName(this,
        "Open PK Archive", lastDir,
        "PK Archives (*.pk);;All Files (*)");

    if (!path.isEmpty()) {
        settings.setValue("pk_last_open_dir", QFileInfo(path).absolutePath());
        openFile(path);
    }
}

void MainWindow::onSetClientRoot() {
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Client Root", clientRoot_,
        QFileDialog::DontUseNativeDialog);
    if (dir.isEmpty()) return;

    QSettings settings;
    settings.setValue("pk_client_root", dir);
    loadClientRoot(dir);
}

void MainWindow::loadManifest() {
    crcMap_.clear();
    if (clientRoot_.isEmpty()) return;

    QStringList tryPaths = {
        clientRoot_ + "/versions/trunk.txt",
        clientRoot_ + "/client/versions/trunk.txt",
    };

    for (const auto& path : tryPaths) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        bool inFiles = false;
        while (!f.atEnd()) {
            QString line = f.readLine().trimmed();
            if (line == "[files]") { inFiles = true; continue; }
            if (line.startsWith('[')) { inFiles = false; continue; }
            if (!inFiles || line.isEmpty()) continue;

            int comma = line.indexOf(',');
            if (comma <= 0) continue;
            QString filePath = line.left(comma);

            // CRC-32/MPEG-2 of lowercase backslash path + 4 null bytes.
            // Matching DarkflameServer AssetManager::crc32b() with NULL_TERMINATOR.
            std::string normalized = filePath.toLower().toStdString();
            std::replace(normalized.begin(), normalized.end(), '/', '\\');
            if (normalized.rfind("client\\res\\", 0) != 0)
                normalized = "client\\res\\" + normalized;

            uint32_t crc = computePackCrc(normalized);
            crcMap_[crc] = filePath.toStdString();
        }
        if (!crcMap_.empty()) break;
    }
}

void MainWindow::loadPki() {
    pkiPackPaths_.clear();
    crcToPackIdx_.clear();
    if (clientRoot_.isEmpty()) return;

    // PKI binary format (version 3):
    //   u32 version
    //   u32 pack_count
    //   For each pack: u32 string_len + string (backslash-separated path)
    //   u32 entry_count
    //   For each entry (20 bytes): u32 crc, s32 lower, s32 upper, u32 pack_index, u32 unknown
    QStringList tryPaths = {
        clientRoot_ + "/versions/primary.pki",
        clientRoot_ + "/client/versions/primary.pki",
    };

    for (const auto& path : tryPaths) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;

        QByteArray raw = f.readAll();
        const uint8_t* d = reinterpret_cast<const uint8_t*>(raw.constData());
        size_t size = static_cast<size_t>(raw.size());
        size_t off = 0;

        auto read_u32 = [&]() -> uint32_t {
            if (off + 4 > size) return 0;
            uint32_t v = d[off] | (d[off+1]<<8) | (d[off+2]<<16) | (static_cast<uint32_t>(d[off+3])<<24);
            off += 4;
            return v;
        };

        uint32_t version = read_u32();
        if (version < 1 || version > 10) continue; // sanity check

        uint32_t packCount = read_u32();
        if (packCount > 10000) continue;

        for (uint32_t i = 0; i < packCount; ++i) {
            uint32_t slen = read_u32();
            if (off + slen > size) break;
            std::string name(reinterpret_cast<const char*>(d + off), slen);
            off += slen;
            // Normalize to forward slashes
            std::replace(name.begin(), name.end(), '\\', '/');
            pkiPackPaths_.push_back(name);
        }

        uint32_t entryCount = read_u32();
        if (off + entryCount * 20 > size) break;

        for (uint32_t i = 0; i < entryCount; ++i) {
            uint32_t crc = read_u32();
            read_u32(); // lower_crc
            read_u32(); // upper_crc
            uint32_t packIdx = read_u32();
            read_u32(); // unknown

            if (packIdx < pkiPackPaths_.size()) {
                crcToPackIdx_[crc] = static_cast<int>(packIdx);
            }
        }

        if (!pkiPackPaths_.empty()) break;
    }
}

void MainWindow::buildTree() {
    tree_->clear();
    if (packs_.empty()) return;

    bool multiPack = packs_.size() > 1;

    for (int pi = 0; pi < static_cast<int>(packs_.size()); ++pi) {
        const auto& pack = packs_[pi];

        QTreeWidgetItem* packRoot = nullptr;
        if (multiPack) {
            packRoot = new QTreeWidgetItem(tree_);
            packRoot->setText(0, pack.name);
            packRoot->setText(1, QString("%1 entries").arg(pack.archive->entry_count()));
            packRoot->setData(0, Qt::UserRole, pi);
            packRoot->setData(0, Qt::UserRole + 1, -1); // not an entry
        }

        pack.archive->for_each([&](size_t index, const lu::assets::PackIndexEntry& entry) {
            QTreeWidgetItem* parent = multiPack ? packRoot : nullptr;
            auto* item = multiPack
                ? new QTreeWidgetItem(parent)
                : new QTreeWidgetItem(tree_);

            QString name = resolveFilename(entry.crc);
            if (name.isEmpty())
                name = QString("0x%1").arg(entry.crc, 8, 16, QChar('0'));

            item->setText(0, name);
            item->setText(1, formatSize(entry.uncompressed_size));
            item->setText(2, (entry.is_compressed & 1)
                ? formatSize(entry.compressed_size) : "---");
            // Detect type from resolved filename extension
            QString type;
            if (!name.startsWith("0x")) {
                QString ext = QFileInfo(name).suffix().toLower();
                if      (ext == "nif" || ext == "kf" || ext == "kfm") type = "NIF";
                else if (ext == "hkx")       type = "HKX";
                else if (ext == "dds")       type = "DDS";
                else if (ext == "tga")       type = "TGA";
                else if (ext == "fdb")       type = "FDB";
                else if (ext == "fsb")       type = "FSB";
                else if (ext == "fev")       type = "FEV";
                else if (ext == "lua")       type = "LUA";
                else if (ext == "xml" || ext == "lxfml" || ext == "lutriggers"
                         || ext == "aud" || ext == "paxml") type = "XML";
                else if (ext == "gfx" || ext == "swf") type = "GFX";
                else if (ext == "psb")       type = "PSB";
                else if (ext == "raw")       type = "RAW";
                else if (ext == "luz")       type = "LUZ";
                else if (ext == "lvl")       type = "LVL";
                else if (ext == "zal")       type = "ZAL";
                else if (ext == "ast")       type = "AST";
                else if (ext == "settings")  type = "SET";
                else if (ext == "fxo" || ext == "fxp") type = "FXO";
                else if (ext == "g" || ext == "g1" || ext == "g2") type = "G";
                else if (ext == "scm")       type = "SCM";
                else if (ext == "cfg" || ext == "ini" || ext == "txt") type = "TXT";
                else if (ext == "jpg" || ext == "jpeg") type = "JPEG";
                else if (ext == "png")       type = "PNG";
                else if (ext == "mp3")       type = "MP3";
                else if (ext == "wav")       type = "WAV";
                else if (ext == "dll" || ext == "exe") type = "EXE";
                else if (ext == "cur")       type = "CUR";
                else if (!ext.isEmpty())     type = ext.toUpper();
            }
            item->setText(3, type);
            item->setText(4, QString("0x%1").arg(entry.data_offset, 0, 16));

            item->setData(0, Qt::UserRole, pi);
            item->setData(0, Qt::UserRole + 1, static_cast<int>(index));

            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        });
    }

    if (!multiPack) {
        tree_->sortByColumn(0, Qt::AscendingOrder);
    }
}

QString MainWindow::resolveFilename(uint32_t crc) const {
    auto it = crcMap_.find(crc);
    if (it != crcMap_.end())
        return QString::fromStdString(it->second);
    return {};
}

QString MainWindow::detectFileType(const std::vector<uint8_t>& data) const {
    if (data.size() < 4) return "?";

    uint32_t m32 = data[0] | (data[1] << 8) | (data[2] << 16) | (static_cast<uint32_t>(data[3]) << 24);

    // Magic byte detection
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return "PNG";
    if (data[0] == 'D' && data[1] == 'D' && data[2] == 'S' && data[3] == ' ') return "DDS";
    if (data[0] == 'G' && data[1] == 'a' && data[2] == 'm' && data[3] == 'e') return "NIF";
    if (data[0] == 'N' && data[1] == 'e' && data[2] == 't' && data[3] == 'I') return "NIF";
    if (data[0] == ';' && data[1] == 'G' && data[2] == 'a' && data[3] == 'm') return "KFM";
    if (data[0] == 'F' && data[1] == 'S' && data[2] == 'B') return "FSB";
    if (data[0] == 'F' && data[1] == 'E' && data[2] == 'V') return "FEV";
    if (data[0] == 'n' && data[1] == 'd' && data[2] == 'p' && data[3] == 'k') return "PK";
    if (data[0] == 's' && data[1] == 'd' && data[2] == '0') return "SD0";
    if (data[0] == 'C' && data[1] == 'F' && data[2] == 'X') return "GFX";
    if (data[0] == 'F' && data[1] == 'W' && data[2] == 'S') return "GFX";
    if (data[0] == 'C' && data[1] == 'W' && data[2] == 'S') return "GFX";
    if (data[0] == 0xFF && data[1] == 0xD8) return "JPEG";
    if (data[0] == '<') return "XML";
    if (m32 == 0x42420D31) return "G";      // brick geometry
    if (m32 == 0x57E0E057) return "HKX";    // havok binary
    if (m32 == 4) return "FDB";              // FDB starts with table count (usually small u32)
    if (data.size() >= 8 && data[4] == 0 && data[5] == 0 && data[6] == 0 && data[7] == 0
        && m32 > 0 && m32 < 200) return "FDB"; // likely FDB (table count < 200, then null)

    // Lua bytecode
    if (data.size() >= 4 && data[0] == 0x1B && data[1] == 'L' && data[2] == 'u' && data[3] == 'a') return "LUA";
    // Lua source
    if (data.size() >= 2 && data[0] == '-' && data[1] == '-') return "LUA";

    bool isText = true;
    for (size_t i = 0; i < std::min(data.size(), size_t(64)); ++i) {
        if (data[i] == 0) { isText = false; break; }
    }
    if (isText) return "TXT";
    return "BIN";
}

QString MainWindow::formatSize(uint32_t bytes) const {
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

void MainWindow::extractEntry(int packIdx, int entryIdx, const QString& destPath) {
    if (packIdx < 0 || packIdx >= static_cast<int>(packs_.size())) return;
    try {
        auto data = packs_[packIdx].archive->extract(static_cast<size_t>(entryIdx));
        QFile out(destPath);
        if (!out.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, "Error",
                QString("Could not write:\n%1").arg(destPath));
            return;
        }
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<qint64>(data.size()));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "Error",
            QString("Extraction failed:\n%1").arg(e.what()));
    }
}

void MainWindow::onExtractSelected() {
    auto items = tree_->selectedItems();
    if (items.isEmpty()) {
        QMessageBox::information(this, "No Selection", "Select entries to extract.");
        return;
    }

    // Filter to actual entries (not pack root nodes)
    QList<QTreeWidgetItem*> entries;
    for (auto* item : items) {
        if (item->data(0, Qt::UserRole + 1).toInt() >= 0)
            entries.append(item);
    }
    if (entries.isEmpty()) return;

    if (entries.size() == 1) {
        int pi = entries[0]->data(0, Qt::UserRole).toInt();
        int ei = entries[0]->data(0, Qt::UserRole + 1).toInt();
        QString name = entries[0]->text(0);
        if (name.startsWith("0x")) name = QString("entry_%1.bin").arg(ei);
        // Use just the filename part
        name = QFileInfo(name).fileName();

        QString path = qt_common::FileBrowserDialog::getSaveFileName(this,
            "Extract File", name, "All Files (*)");
        if (!path.isEmpty()) {
            extractEntry(pi, ei, path);
            statusBar()->showMessage(QString("Extracted: %1").arg(path), 5000);
        }
    } else {
        QString dir = QFileDialog::getExistingDirectory(
            this, "Extract to Directory", QString(),
            QFileDialog::DontUseNativeDialog);
        if (dir.isEmpty()) return;

        int count = 0;
        for (auto* item : entries) {
            int pi = item->data(0, Qt::UserRole).toInt();
            int ei = item->data(0, Qt::UserRole + 1).toInt();
            QString name = item->text(0);
            if (name.startsWith("0x")) name = QString("entry_%1.bin").arg(ei);

            QString safeName = name;
            safeName.replace('\\', '/');
            QFileInfo fi(dir + "/" + safeName);
            fi.absoluteDir().mkpath(".");
            extractEntry(pi, ei, fi.absoluteFilePath());
            count++;
        }
        statusBar()->showMessage(
            QString("Extracted %1 files to %2").arg(count).arg(dir), 5000);
    }
}

void MainWindow::onExtractAll() {
    if (packs_.empty()) return;

    QString dir = QFileDialog::getExistingDirectory(
        this, "Extract All to Directory", QString(),
        QFileDialog::DontUseNativeDialog);
    if (dir.isEmpty()) return;

    int count = 0;
    for (int pi = 0; pi < static_cast<int>(packs_.size()); ++pi) {
        packs_[pi].archive->for_each([&](size_t index, const lu::assets::PackIndexEntry& entry) {
            QString name = resolveFilename(entry.crc);
            if (name.isEmpty())
                name = QString("%1/entry_%2.bin").arg(packs_[pi].name).arg(index);

            QString safeName = name;
            safeName.replace('\\', '/');
            QFileInfo fi(dir + "/" + safeName);
            fi.absoluteDir().mkpath(".");
            extractEntry(pi, static_cast<int>(index), fi.absoluteFilePath());
            count++;
        });
    }

    statusBar()->showMessage(
        QString("Extracted all %1 files to %2").arg(count).arg(dir), 10000);
}

void MainWindow::onSearchChanged(const QString& text) {
    QString lower = text.toLower();

    // For multi-pack mode, handle parent visibility
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        auto* top = tree_->topLevelItem(i);
        if (top->data(0, Qt::UserRole + 1).toInt() < 0) {
            // Pack root — show if any child matches
            bool anyVisible = false;
            for (int j = 0; j < top->childCount(); ++j) {
                auto* child = top->child(j);
                bool match = lower.isEmpty() ||
                             child->text(0).toLower().contains(lower) ||
                             child->text(3).toLower().contains(lower);
                child->setHidden(!match);
                if (match) anyVisible = true;
            }
            top->setHidden(!anyVisible && !lower.isEmpty());
        } else {
            // Single-pack mode entry
            bool match = lower.isEmpty() ||
                         top->text(0).toLower().contains(lower) ||
                         top->text(3).toLower().contains(lower);
            top->setHidden(!match);
        }
    }
}

void MainWindow::onItemDoubleClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) return;
    int pi = item->data(0, Qt::UserRole).toInt();
    int ei = item->data(0, Qt::UserRole + 1).toInt();
    if (ei < 0 || pi < 0 || pi >= static_cast<int>(packs_.size())) return;

    // Detect type if not already set
    if (item->text(3).isEmpty()) {
        try {
            auto data = packs_[pi].archive->extract(static_cast<size_t>(ei));
            item->setText(3, detectFileType(data));
        } catch (...) {}
    }

    statusBar()->showMessage(
        QString("%1: %2, type: %3")
            .arg(item->text(0), item->text(1), item->text(3)), 5000);
}

void MainWindow::onContextMenu(const QPoint& pos) {
    auto* item = tree_->itemAt(pos);
    if (!item) return;

    QMenu menu;
    menu.addAction("Extract...", this, &MainWindow::onExtractSelected);

    int ei = item->data(0, Qt::UserRole + 1).toInt();
    if (ei >= 0) {
        auto* detectAction = menu.addAction("Detect Type");
        connect(detectAction, &QAction::triggered, [this, item]() {
            int pi = item->data(0, Qt::UserRole).toInt();
            int idx = item->data(0, Qt::UserRole + 1).toInt();
            if (pi < 0 || pi >= static_cast<int>(packs_.size())) return;
            try {
                auto data = packs_[pi].archive->extract(static_cast<size_t>(idx));
                item->setText(3, detectFileType(data));
            } catch (...) {}
        });
    }

    menu.exec(tree_->viewport()->mapToGlobal(pos));
}



} // namespace pk_viewer
