#include "stylehelper.h"

QString StyleHelper::commonTableStyle()
{
    return
        "QTableWidget {"
        "    background-color: #0f1419;"
        "    border: 2px solid #2d3a4a;"
        "    border-radius: 8px;"
        "    color: #ecf0f1;"
        "    gridline-color: #2d3a4a;"
        "}"
        "QTableWidget::item {"
        "    padding: 8px;"
        "    border-bottom: 1px solid #2d3a4a;"
        "}"
        "QTableWidget::item:selected {"
        "    background-color: #e94560;"
        "    color: white;"
        "}"
        "QHeaderView::section {"
        "    background-color: #16213e;"
        "    color: #ecf0f1;"
        "    padding: 10px;"
        "    border: none;"
        "    border-bottom: 2px solid #e94560;"
        "    font-weight: bold;"
        "}";
}

QString StyleHelper::commonButtonStyle()
{
    return
        "QPushButton {"
        "    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #e94560, stop:1 #c73e54);"
        "    color: white;"
        "    border: none;"
        "    border-radius: 6px;"
        "    padding: 10px 20px;"
        "    font-weight: bold;"
        "    font-size: 13px;"
        "    min-width: 80px;"
        "}"
        "QPushButton:hover {"
        "    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #ff6b6b, stop:1 #e94560);"
        "}"
        "QPushButton:pressed {"
        "    background: #a03045;"
        "}";
}

QString StyleHelper::commonInputStyle()
{
    return
        "QLineEdit {"
        "    background-color: #0f1419;"
        "    border: 2px solid #2d3a4a;"
        "    border-radius: 6px;"
        "    padding: 8px;"
        "    color: #ecf0f1;"
        "    selection-background-color: #e94560;"
        "}"
        "QLineEdit:focus {"
        "    border-color: #e94560;"
        "}";
}

QString StyleHelper::commonGroupBoxStyle()
{
    return
        "QGroupBox {"
        "    border: 2px solid #e94560;"
        "    border-radius: 8px;"
        "    margin-top: 15px;"
        "    padding-top: 15px;"
        "    font-weight: bold;"
        "    color: #ecf0f1;"
        "}"
        "QGroupBox::title {"
        "    subcontrol-origin: margin;"
        "    left: 15px;"
        "    padding: 0 10px;"
        "    color: #e94560;"
        "    font-size: 16px;"
        "}";
}

QString StyleHelper::fullCommonStyle()
{
    return
        "QWidget {"
        "    background-color: #1a1a2e;"
        "    color: #ecf0f1;"
        "}"
        "QLabel {"
        "    color: #ecf0f1;"
        "    font-size: 16px;"
        "    font-weight: bold;"
        "}"
        + commonTableStyle()
        + commonButtonStyle()
        + commonInputStyle()
        + commonGroupBoxStyle();
}
