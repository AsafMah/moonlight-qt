#pragma once

#include <QTextStream>

// Needs a QGuiApplication: the last test measures the overlay's real fonts
bool runPanelLayoutTests(QTextStream& err);
