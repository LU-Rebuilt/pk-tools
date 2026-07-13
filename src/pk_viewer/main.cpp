#include "main_window.h"
#include <QApplication>
#include <QFileInfo>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setOrganizationName("LU-Rebuilt");
    app.setApplicationName("PkViewer");

    pk_viewer::MainWindow window;
    window.show();

    if (argc > 1) {
        // A directory argument opens it as a folder of loose .sd0 files (the same
        // path as the "Open SD0 Folder..." menu action); a file argument opens it
        // as a PK archive.
        QString arg = QString::fromUtf8(argv[1]);
        if (QFileInfo(arg).isDir()) {
            window.loadSd0Folder(arg);
        } else {
            window.openFile(arg);
        }
    }

    return app.exec();
}
