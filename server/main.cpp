#include "mainwindow.h"
#include "crypto.h"
#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    if (crypto_init(CRYPTO_KEY_FILE) != 0) {
        fprintf(stderr, "crypto module init failed, exiting\n");
        return -1;
    }

    MainWindow w;
    w.show();
    return a.exec();
}