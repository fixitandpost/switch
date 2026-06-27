#pragma once

#include <QByteArray>
#include <QString>
#include <QVariant>
#include <QVector>

enum class SwitchOscArgumentType {
	Int,
	Float,
	String,
	Blob,
	True,
	False,
	Null,
	Infinity,
};

struct SwitchOscArgument {
	SwitchOscArgumentType type = SwitchOscArgumentType::String;
	QVariant value;
};

struct SwitchOscMessage {
	QString address;
	QVector<SwitchOscArgument> arguments;
};

QString SwitchOscArgumentTypeName(SwitchOscArgumentType type);
QString SwitchOscArgumentsToConfigString(const QVector<SwitchOscArgument> &arguments);
QString SwitchOscArgumentsToSummaryString(const QVector<SwitchOscArgument> &arguments);
QString SwitchOscMessageSummary(const SwitchOscMessage &message);
bool SwitchParseOscArguments(const QString &input, QVector<SwitchOscArgument> *arguments, QString *error = nullptr);
bool SwitchOscArgumentsEqual(const QVector<SwitchOscArgument> &lhs, const QVector<SwitchOscArgument> &rhs);
bool SwitchBuildOscPacket(const SwitchOscMessage &message, QByteArray *packet, QString *error = nullptr);
bool SwitchParseOscPacket(const QByteArray &packet, SwitchOscMessage *message, QString *error = nullptr);
bool SwitchOscAddressPatternMatches(const QString &pattern, const QString &address);
