#pragma once

#include <QtLogging>
#include <QString>

void customMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg);
