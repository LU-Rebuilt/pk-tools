#include "main_window.h"
#include "file_browser.h"

#include "gamebryo/nif/nif_reader.h"
#include "gamebryo/kfm/kfm_reader.h"
#include "gamebryo/settings/settings_reader.h"
#include "netdevil/zone/luz/luz_reader.h"
#include "netdevil/zone/lvl/lvl_reader.h"
#include "netdevil/zone/lvl/lvl_writer.h"
#include "microsoft/tga/tga_reader.h"
#include "microsoft/tga/tga_writer.h"
#include "netdevil/common/ldf/ldf_reader.h"
#include "forkparticle/psb/psb_reader.h"
#include "lego/brick_geometry/brick_geometry.h"

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
#include <cstring>
#include <cctype>
#include <string_view>

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
    fileMenu->addAction("Open SD0 &Folder...",
                        this, &MainWindow::onOpenSd0Folder);
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

void MainWindow::onOpenSd0Folder() {
    QSettings settings;
    QString lastDir = settings.value("pk_last_sd0_dir").toString();

    QString dir = QFileDialog::getExistingDirectory(
        this, "Open Folder of .sd0 Files", lastDir,
        QFileDialog::DontUseNativeDialog);
    if (dir.isEmpty()) return;

    settings.setValue("pk_last_sd0_dir", dir);
    loadSd0Folder(dir);
}

void MainWindow::loadSd0Folder(const QString& dir) {
    // Recursively find every .sd0 file under dir, decompress each one, and show it in
    // the tree the same way a packed entry would appear — one LoadedPack per file (so
    // extraction/type-detection/search all work unchanged), with detectFileType run
    // against the decompressed bytes to identify what format each one actually holds.
    QStringList sd0Files;
    QDirIterator it(dir, QStringList() << "*.sd0", QDir::Files,
                     QDirIterator::Subdirectories);
    while (it.hasNext()) sd0Files << it.next();
    sd0Files.sort();

    if (sd0Files.isEmpty()) {
        QMessageBox::information(this, "No SD0 Files",
            QString("No .sd0 files found under:\n%1").arg(dir));
        return;
    }

    QProgressDialog progress("Decompressing SD0 files...", "Cancel", 0, sd0Files.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);

    std::vector<LoadedPack> results;
    results.reserve(static_cast<size_t>(sd0Files.size()));
    int failed = 0;

    for (int i = 0; i < sd0Files.size(); ++i) {
        progress.setValue(i);
        if (progress.wasCanceled()) break;

        const QString& path = sd0Files[i];
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) { failed++; continue; }
        QByteArray raw = f.readAll();

        LoadedPack pack;
        pack.path = path;
        QString baseName = QFileInfo(path).fileName();
        // Strip the .sd0 suffix so the tree/extraction show the file's real name
        // (e.g. "level.lvl.sd0" -> "level.lvl") — matches how packed entries are shown.
        pack.name = baseName.endsWith(".sd0", Qt::CaseInsensitive)
            ? baseName.left(baseName.size() - 4) : baseName;
        pack.loose_original_name = pack.name;

        try {
            pack.loose_data = lu::assets::sd0_decompress(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(raw.constData()),
                static_cast<size_t>(raw.size())));
        } catch (const std::exception&) {
            failed++;
            continue;
        }

        results.push_back(std::move(pack));
    }
    progress.setValue(sd0Files.size());

    packs_ = std::move(results);
    clientRoot_.clear();
    crcMap_.clear();
    pki_ = lu::assets::PkiFile{};

    setWindowTitle(QString("PK Viewer - %1 (SD0 folder)").arg(QFileInfo(dir).fileName()));
    buildTree();

    QString msg = QString("Loaded %1 SD0 file(s) from %2").arg(packs_.size()).arg(dir);
    if (failed > 0) msg += QString(" (%1 failed to decompress)").arg(failed);
    statusLabel_->setText(msg);
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
    pki_ = lu::assets::PkiFile{};
    if (clientRoot_.isEmpty()) return;

    QStringList tryPaths = {
        clientRoot_ + "/versions/primary.pki",
        clientRoot_ + "/client/versions/primary.pki",
    };

    for (const auto& path : tryPaths) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;

        QByteArray raw = f.readAll();
        auto parsed = lu::assets::pki_parse(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(raw.constData()),
            static_cast<size_t>(raw.size())));
        if (parsed.pack_paths.empty()) continue;

        pki_ = std::move(parsed);
        break;
    }
}

void MainWindow::buildTree() {
    tree_->clear();
    if (packs_.empty()) return;

    // Loose SD0-folder mode: each pack IS a single decompressed file (archive == nullptr),
    // so there's no pack-root/entry nesting — one flat row per file, entry index always 0.
    // The on-disk name is just a content hash, so column 0 stays the hash (for locating
    // the source .sd0) while column 4 (unused here — no pack offset applies) surfaces
    // whatever real name detectFile() recovered from the file's own content.
    if (!packs_[0].archive) {
        tree_->setHeaderLabels({"Filename (hash)", "Size", "Compressed", "Type", "Detected Name"});
        for (int pi = 0; pi < static_cast<int>(packs_.size()); ++pi) {
            const auto& pack = packs_[pi];
            DetectedFile detected = detectFile(pack.loose_data);

            auto* item = new QTreeWidgetItem(tree_);
            item->setText(0, pack.loose_original_name);
            item->setText(1, formatSize(static_cast<uint32_t>(pack.loose_data.size())));
            item->setText(2, "---");
            item->setText(3, detected.type);
            item->setText(4, detected.name);
            item->setData(0, Qt::UserRole, pi);
            item->setData(0, Qt::UserRole + 1, 0); // single synthetic entry
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        }
        tree_->sortByColumn(3, Qt::AscendingOrder); // group by detected type by default
        return;
    }

    tree_->setHeaderLabels({"Filename / CRC", "Size", "Compressed", "Type", "Offset"});
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
    return detectFile(data).type;
}

namespace {

bool starts_with(const std::vector<uint8_t>& d, std::string_view s) {
    return d.size() >= s.size() && std::memcmp(d.data(), s.data(), s.size()) == 0;
}

uint32_t read_u32le(const std::vector<uint8_t>& d, size_t off) {
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off+1]) << 8) |
           (static_cast<uint32_t>(d[off+2]) << 16) | (static_cast<uint32_t>(d[off+3]) << 24);
}

} // anonymous namespace

// Identify the actual format of loose/decompressed file data — used both for PK entries
// (where the manifest already gives a name/extension, so this mostly confirms it) and,
// critically, for the SD0-folder mode where files are hash-named and this is the ONLY
// signal available. Beyond magic bytes, small/cheap formats are structurally verified by
// actually parsing them with the real lu-assets reader (not just sniffing the header),
// and any name the format carries internally (a referenced .raw/.nif path, an FDB table
// list, an NiNode name, ...) is surfaced so hash-named files get something meaningful.
// Large/expensive formats (terrain .raw, FDB, HKX) are only header-peeked, not fully
// parsed, so this stays fast even on the 2000+ files a loose SD0 folder can contain.
DetectedFile MainWindow::detectFile(const std::vector<uint8_t>& data) const {
    if (data.size() < 4) return {"?", ""};

    uint32_t m32 = read_u32le(data, 0);

    // ---- Gamebryo NIF container (.nif/.kf/.etk) — fully parsed: it's small (almost
    // always well under 1MB) and parsing recovers the scene root's name for free. ----
    if (starts_with(data, "Gamebryo File Format") || starts_with(data, "NetImmerse File Format")) {
        try {
            auto nif = lu::assets::nif_parse(std::span<const uint8_t>(data.data(), data.size()));
            QString name;
            if (!nif.nodes.empty() && !nif.nodes.front().name.empty()) {
                name = QString::fromStdString(nif.nodes.front().name);
            }
            // .kf files carry animation (NiControllerSequence), not a scene graph.
            QString type = !nif.sequences.empty() && nif.nodes.empty() ? "KF" : "NIF";
            return {type, name};
        } catch (const std::exception&) {
            return {"NIF?", ""}; // right magic, but didn't parse — flag rather than hide it
        }
    }

    // ---- KFM (keyframe manager) — fully parsed: tiny files, and model_path is exactly
    // the kind of identifying name a hash-named loose file needs. ----
    if (starts_with(data, ";Gamebryo KFM File Version")) {
        try {
            auto kfm = lu::assets::kfm_parse(std::span<const uint8_t>(data.data(), data.size()));
            return {"KFM", QString::fromStdString(kfm.model_path_normalized())};
        } catch (const std::exception&) {
            return {"KFM?", ""};
        }
    }

    // ---- LUZ (zone) — fully parsed: raw_path (the terrain file it references) is a
    // good proxy for the zone's own identity. ----
    if (data.size() >= 4) {
        try {
            auto luz = lu::assets::luz_parse(std::span<const uint8_t>(data.data(), data.size()));
            if (luz.version >= 20 && luz.version <= 60 && !luz.scenes.empty()) {
                return {"LUZ", QString::fromStdString(luz.raw_path)};
            }
        } catch (const std::exception&) {
            // Not a LUZ — most files will hit this; fall through to other checks.
        }
    }

    // ---- Old pre-chunked LVL (no CHNK framing, version < ~38 in shipped clients but
    // seen up to 42/43 in this corpus) — fully parsed. What first looked like an
    // unrecognized "float-table" blob turned out to be complete, valid old-format .lvl
    // files (lvl_parse's old_format path): the "u16==u16" header pair I kept seeing is
    // just header_version/data_version written twice, not a distinct magic. lvl_parse's
    // old-format path has no magic bytes to anchor on, so trust is established by a
    // round-trip: lvl_write(lvl_parse(data)) must reproduce data exactly. That's a much
    // stronger signal than "did it throw" — sequential binary parsers can walk garbage
    // to a plausible-looking EOF; reproducing every byte on the way back out essentially
    // can't happen by chance. (An earlier version of this check required a nonempty
    // object/particle count, which wrongly rejected legitimately empty scenes.) ----
    if (data.size() >= 8) {
        uint16_t hv = static_cast<uint16_t>(data[0] | (data[1] << 8));
        uint16_t dv = static_cast<uint16_t>(data[2] | (data[3] << 8));
        if (hv == dv && hv >= 20 && hv <= 45) {
            try {
                auto lvl = lu::assets::lvl_parse(std::span<const uint8_t>(data.data(), data.size()));
                if (lvl.old_format) {
                    auto rewritten = lu::assets::lvl_write(lvl);
                    if (rewritten.size() == data.size() &&
                        std::memcmp(rewritten.data(), data.data(), data.size()) == 0) {
                        return {"LVL", ""}; // no name carried; raw_path lives in the paired .luz
                    }
                }
            } catch (const std::exception&) {}
        }
    }

    // ---- TGA (structural, no magic bytes at all — has to be attempted rather than
    // sniffed). tga_parse accepts any buffer >= 18 bytes with no sanity check on its own,
    // so a round-trip alone would be a near-tautology (it just replays header+payload
    // bytes verbatim); require a real width/height/bpp combination too. Found by chasing
    // down what turned out to be a real, if oddly-headered, texture: two files that
    // looked like an unknown 44-byte-header raster were actually plain 128x128/64x64
    // 24bpp TGAs (verified via the trailing "TRUEVISION-XFILE." 2.0 footer signature) —
    // tga_parse just has no magic to recognize them by. A third near-identical-looking
    // trio decodes to width=0/bpp=0 at the real TGA field offsets and is correctly
    // rejected here — those are a different, still-unidentified format. ----
    if (data.size() >= 18) {
        try {
            auto tga = lu::assets::tga_parse(std::span<const uint8_t>(data.data(), data.size()));
            bool plausibleBpp = tga.bits_per_pixel == 8 || tga.bits_per_pixel == 16 ||
                               tga.bits_per_pixel == 24 || tga.bits_per_pixel == 32;
            bool plausibleType = tga.image_type <= 3 || (tga.image_type >= 9 && tga.image_type <= 11);
            if (tga.width > 0 && tga.height > 0 && plausibleBpp && plausibleType) {
                auto rewritten = lu::assets::tga_write(tga);
                if (rewritten.size() == data.size() &&
                    std::memcmp(rewritten.data(), data.data(), data.size()) == 0) {
                    return {"TGA", QString("%1x%2").arg(tga.width).arg(tga.height)};
                }
            }
        } catch (const std::exception&) {}
    }

    // ---- .settings (NiKFMTool binary) — u8-length-prefixed version string as the first
    // field (originally spotted as "2.3.0"/"2.3.2", but the reader makes no version
    // assumption, and "2.2.2" files exist too — verify structurally instead of pinning
    // to one version string). ----
    if (data.size() >= 2 && data[0] >= 3 && data[0] <= 16 &&
        static_cast<size_t>(data[0]) + 1 <= data.size() &&
        std::isdigit(data[1]) && data[2] == '.') {
        try {
            lu::assets::settings_parse(std::span<const uint8_t>(data.data(), data.size()));
            return {"SET", ""};
        } catch (const std::exception&) {
            // Fall through — a digit+'.' start doesn't guarantee this is really .settings.
        }
    }

    // ---- Terrain .raw (chunked, version >= 30) — header-only: u16 version, u8 dev,
    // u32 chunk_count/width/height, all in a narrow sane range. Never fully parsed here
    // (files run to tens of MB and this is a browse/identify tool, not a validator). ----
    if (data.size() >= 15) {
        uint16_t rawVersion = static_cast<uint16_t>(data[0] | (data[1] << 8));
        uint8_t rawDev = data[2];
        if (rawVersion >= 30 && rawVersion <= 45 && rawDev == 0) {
            uint32_t chunkCount = read_u32le(data, 3);
            uint32_t chunksW = read_u32le(data, 7);
            uint32_t chunksH = read_u32le(data, 11);
            if (chunkCount > 0 && chunkCount <= 10000 && chunksW > 0 && chunksW <= 200 &&
                chunksH > 0 && chunksH <= 200 && chunkCount == chunksW * chunksH) {
                return {"RAW", ""};
            }
        }
    }

    // ---- Brick geometry (.g/.g1/.g2) — fully parsed: small mesh files (positions,
    // normals, optional UVs/bone weights, triangle indices). ----
    if (m32 == lu::assets::BRICK_GEOM_MAGIC) {
        try {
            lu::assets::brick_geometry_parse(std::span<const uint8_t>(data.data(), data.size()));
            return {"G", ""}; // no name carried in the format itself
        } catch (const std::exception&) {
            return {"G?", ""};
        }
    }

    // ---- FDB (CDClient-style database) — header-only: table_count(u32) followed by a
    // plausible pointer/name-length field. Never fully parsed (files can be 10s of MB). ----
    if (m32 == 4) return {"FDB", ""};
    if (data.size() >= 8 && data[4] == 0 && data[5] == 0 && data[6] == 0 && data[7] == 0
        && m32 > 0 && m32 < 200) {
        return {"FDB", ""};
    }

    // ---- ForkParticle PSB (binary emitter) — header_size is always 80 (0x50) with
    // data_size 420 right after; found by cross-referencing PsbFile's documented layout
    // against an unidentified magic bucket in a real SD0-folder corpus. Carries an
    // optional emitter_name at a file-relative offset (psb_types.h +0x194/+0x198). ----
    if (m32 == 80 && data.size() >= 8 && read_u32le(data, 4) == 420) {
        try {
            auto psb = lu::assets::psb_parse(std::span<const uint8_t>(data.data(), data.size()));
            return {"PSB", QString::fromStdString(psb.emitter_name)};
        } catch (const std::exception&) {
            return {"PSB?", ""};
        }
    }

    // ---- ForkParticle effect (text emitter description) — the sibling text format to
    // PSB; carries the emitter name directly as its first line. ----
    if (starts_with(data, "EMITTERNAME:")) {
        std::string_view text(reinterpret_cast<const char*>(data.data()),
                               std::min(data.size(), size_t(256)));
        size_t nameStart = text.find(':') + 1;
        size_t nameEnd = text.find_first_of("\r\n", nameStart);
        QString name;
        if (nameStart != std::string_view::npos && nameEnd != std::string_view::npos) {
            std::string_view raw = text.substr(nameStart, nameEnd - nameStart);
            // Trim leading/trailing whitespace.
            size_t b = raw.find_first_not_of(' ');
            size_t e = raw.find_last_not_of(' ');
            if (b != std::string_view::npos) name = QString::fromUtf8(raw.substr(b, e - b + 1).data(),
                                                                       static_cast<int>(e - b + 1));
        }
        return {"EFFECT", name};
    }

    // ---- Other magic-identifiable binary formats without a lu-assets reader yet, or
    // where a reader exists but full parsing isn't worth it here. ----
    if (data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') return {"PNG", ""};
    if (starts_with(data, "DDS ")) return {"DDS", ""};
    if (starts_with(data, "FSB")) return {"FSB", ""};
    if (starts_with(data, "FEV")) return {"FEV", ""};
    if (starts_with(data, "ndpk")) return {"PK", ""};       // nested PK archive
    if (starts_with(data, "sd0")) return {"SD0", ""};       // sd0-wrapped-in-sd0 (rare)
    if (starts_with(data, "CFX") || starts_with(data, "FWS") || starts_with(data, "CWS"))
        return {"GFX", ""};                                  // Scaleform GFX/SWF
    if (data[0] == 0xFF && data[1] == 0xD8) return {"JPEG", ""};
    if (m32 == 0x57E0E057) return {"HKX", ""};              // Havok packfile
    if (starts_with(data, "\x1BLua")) return {"LUA", ""};   // Lua bytecode
    // Havok "tagfile" format: no fixed magic, but every known .hkx tagfile in this corpus
    // contains "hkRootLevelContainer" (or another hk*Container/hkClass tag) near the
    // start of the section-tag stream.
    {
        std::string_view head(reinterpret_cast<const char*>(data.data()),
                               std::min(data.size(), size_t(256)));
        if (head.find("hkRootLevelContainer") != std::string_view::npos ||
            head.find("hkClass") != std::string_view::npos) {
            return {"HKX", ""}; // tagfile variant (distinct on-disk layout from the packfile above)
        }
    }
    if (starts_with(data, "PK\x03\x04")) return {"ZIP", ""};
    if (starts_with(data, "8BPS")) return {"PSD", ""};
    if (data.size() >= 3 && data[0] == 0x1F && data[1] == 0x8B && data[2] == 0x08) return {"GZIP", ""};
    if (starts_with(data, "MZ")) return {"EXE", ""}; // PE executable/DLL

    // ---- NFF (NetDevil bitmap font) — magic bytes "NFF\0" (0x0046464E as a little-endian
    // u32) + version(u32) + font-name string(u32 len + chars) + point size(u32) + a
    // glyph/charmap table. No lu-assets reader exists (found via Ghidra RE of the client's
    // font loader — the call chain has no named symbols beyond the top-level magic check,
    // so this is magic-only detection, not a full parser); the font name is a real, useful
    // display name straight from the header. ----
    if (data.size() >= 12 && read_u32le(data, 0) == 0x0046464Eu) {
        uint32_t nameLen = read_u32le(data, 8);
        QString name;
        if (nameLen > 0 && nameLen <= 256 && 12 + nameLen <= data.size()) {
            name = QString::fromUtf8(reinterpret_cast<const char*>(data.data() + 12), nameLen);
        }
        return {"NFF", name};
    }

    // ---- XML-shaped text formats: distinguish by root element instead of a bare "XML"
    // bucket, since .aud/.lutriggers/.lxfml are all XML but semantically distinct. ----
    if (data[0] == '<' || (data.size() > 3 && data[0]=='\xEF' && data[1]=='\xBB' && data[2]=='\xBF')) {
        std::string_view text(reinterpret_cast<const char*>(data.data()),
                               std::min(data.size(), size_t(512)));
        if (text.find("<SceneAudioAttributes") != std::string_view::npos) return {"AUD", ""};
        if (text.find("<triggers") != std::string_view::npos) return {"LUTRIGGERS", ""};
        if (text.find("<LXFML") != std::string_view::npos) return {"LXFML", ""};
        return {"XML", ""};
    }

    // Lua source (heuristic — "--" comment prefix)
    if (data.size() >= 2 && data[0] == '-' && data[1] == '-') return {"LUA", ""};

    // ---- Serialized FMOD category-tree / mixer snapshot fragment — a different on-disk
    // string convention (u32 count + u32 char_count + char_count x plain ASCII bytes, no
    // UTF-16), matching FevEventCategory's "master/..." category paths but as a standalone
    // cache file rather than embedded in a .fev project. Heuristic scan, same caveat as
    // above. ----
    if (data.size() >= 8) {
        uint32_t count = read_u32le(data, 0);
        uint32_t strLen = read_u32le(data, 4);
        if (count > 0 && count < 100000 && strLen > 0 && strLen <= 256 &&
            8 + strLen <= data.size()) {
            bool printable = true;
            for (uint32_t c = 0; c < strLen; ++c) {
                uint8_t ch = data[8 + c];
                if (ch != 0 && (ch < 32 || ch > 126)) { printable = false; break; }
            }
            std::string_view text(reinterpret_cast<const char*>(data.data() + 8), strLen);
            if (printable && text.find("master/") != std::string_view::npos) {
                return {"FMOD-fragment", QString::fromUtf8(text.data(),
                                                            static_cast<int>(text.size()))};
            }
        }
    }

    bool isText = true;
    for (size_t i = 0; i < std::min(data.size(), size_t(64)); ++i) {
        if (data[i] == 0) { isText = false; break; }
    }
    if (isText) return {"TXT", ""};
    return {"BIN", ""};
}

QString MainWindow::formatSize(uint32_t bytes) const {
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
}

void MainWindow::extractEntry(int packIdx, int entryIdx, const QString& destPath) {
    if (packIdx < 0 || packIdx >= static_cast<int>(packs_.size())) return;
    try {
        const auto& pack = packs_[packIdx];
        std::vector<uint8_t> extracted;
        const std::vector<uint8_t>* data = &pack.loose_data;
        if (pack.archive) {
            extracted = pack.archive->extract(static_cast<size_t>(entryIdx));
            data = &extracted;
        }
        QFile out(destPath);
        if (!out.open(QIODevice::WriteOnly)) {
            QMessageBox::warning(this, "Error",
                QString("Could not write:\n%1").arg(destPath));
            return;
        }
        out.write(reinterpret_cast<const char*>(data->data()),
                  static_cast<qint64>(data->size()));
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
        if (!packs_[pi].archive) {
            // Loose SD0-folder mode: one synthetic entry (index 0) per pack.
            QFileInfo fi(dir + "/" + packs_[pi].loose_original_name);
            fi.absoluteDir().mkpath(".");
            extractEntry(pi, 0, fi.absoluteFilePath());
            count++;
            continue;
        }
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
                             child->text(3).toLower().contains(lower) ||
                             child->text(4).toLower().contains(lower);
                child->setHidden(!match);
                if (match) anyVisible = true;
            }
            top->setHidden(!anyVisible && !lower.isEmpty());
        } else {
            // Single-pack mode entry (also covers loose SD0-folder rows, whose recovered
            // content name lives in column 4 — the only searchable name they have besides
            // their content hash).
            bool match = lower.isEmpty() ||
                         top->text(0).toLower().contains(lower) ||
                         top->text(3).toLower().contains(lower) ||
                         top->text(4).toLower().contains(lower);
            top->setHidden(!match);
        }
    }
}

void MainWindow::onItemDoubleClicked(QTreeWidgetItem* item, int /*column*/) {
    if (!item) return;
    int pi = item->data(0, Qt::UserRole).toInt();
    int ei = item->data(0, Qt::UserRole + 1).toInt();
    if (ei < 0 || pi < 0 || pi >= static_cast<int>(packs_.size())) return;

    // Detect type if not already set (loose SD0 packs already have it set in buildTree)
    if (item->text(3).isEmpty() && packs_[pi].archive) {
        try {
            auto data = packs_[pi].archive->extract(static_cast<size_t>(ei));
            DetectedFile detected = detectFile(data);
            item->setText(3, detected.type);
            if (!detected.name.isEmpty()) item->setText(4, detected.name);
        } catch (...) {}
    }

    statusBar()->showMessage(
        QString("%1: %2, type: %3%4")
            .arg(item->text(0), item->text(1), item->text(3),
                 item->text(4).isEmpty() ? QString() : QString(" (%1)").arg(item->text(4))),
        5000);
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
                const auto& pack = packs_[pi];
                DetectedFile detected = pack.archive
                    ? detectFile(pack.archive->extract(static_cast<size_t>(idx)))
                    : detectFile(pack.loose_data);
                item->setText(3, detected.type);
                if (!detected.name.isEmpty()) item->setText(4, detected.name);
            } catch (...) {}
        });
    }

    menu.exec(tree_->viewport()->mapToGlobal(pos));
}



} // namespace pk_viewer
