#include "switch-osc.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <limits>

namespace {
QString TypeTagForArgument(const SwitchOscArgument &argument)
{
	switch (argument.type) {
	case SwitchOscArgumentType::Int:
		return QStringLiteral("i");
	case SwitchOscArgumentType::Float:
		return QStringLiteral("f");
	case SwitchOscArgumentType::String:
		return QStringLiteral("s");
	case SwitchOscArgumentType::Blob:
		return QStringLiteral("b");
	case SwitchOscArgumentType::True:
		return QStringLiteral("T");
	case SwitchOscArgumentType::False:
		return QStringLiteral("F");
	case SwitchOscArgumentType::Null:
		return QStringLiteral("N");
	case SwitchOscArgumentType::Infinity:
		return QStringLiteral("I");
	}
	return QStringLiteral("s");
}

void AppendPaddedString(QByteArray &buffer, const QString &value)
{
	QByteArray utf8 = value.toUtf8();
	buffer.append(utf8);
	buffer.append('\0');
	while ((buffer.size() % 4) != 0)
		buffer.append('\0');
}

void AppendInt32(QByteArray &buffer, qint32 value)
{
	const quint32 bigEndian = qToBigEndian<quint32>(static_cast<quint32>(value));
	buffer.append(reinterpret_cast<const char *>(&bigEndian), 4);
}

void AppendFloat32(QByteArray &buffer, float value)
{
	quint32 bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	bits = qToBigEndian(bits);
	buffer.append(reinterpret_cast<const char *>(&bits), 4);
}

void AppendBlob(QByteArray &buffer, const QByteArray &blob)
{
	AppendInt32(buffer, blob.size());
	buffer.append(blob);
	while ((buffer.size() % 4) != 0)
		buffer.append('\0');
}

bool ReadInt32(const QByteArray &buffer, int &offset, qint32 *value)
{
	if (!value || offset < 0 || offset + 4 > buffer.size())
		return false;

	quint32 raw = 0;
	std::memcpy(&raw, buffer.constData() + offset, 4);
	offset += 4;
	*value = static_cast<qint32>(qFromBigEndian(raw));
	return true;
}

bool ReadFloat32(const QByteArray &buffer, int &offset, float *value)
{
	if (!value || offset < 0 || offset + 4 > buffer.size())
		return false;

	quint32 raw = 0;
	std::memcpy(&raw, buffer.constData() + offset, 4);
	offset += 4;
	raw = qFromBigEndian(raw);
	std::memcpy(value, &raw, sizeof(raw));
	return true;
}

bool ReadPaddedString(const QByteArray &buffer, int &offset, QString *value)
{
	if (!value || offset < 0 || offset >= buffer.size())
		return false;

	const int start = offset;
	int end = start;
	while (end < buffer.size() && buffer.at(end) != '\0')
		++end;
	if (end >= buffer.size())
		return false;

	*value = QString::fromUtf8(buffer.constData() + start, end - start);
	offset = end + 1;
	while ((offset % 4) != 0 && offset < buffer.size())
		++offset;
	return true;
}

bool ReadBlob(const QByteArray &buffer, int &offset, QByteArray *blob)
{
	if (!blob)
		return false;

	qint32 size = 0;
	if (!ReadInt32(buffer, offset, &size) || size < 0 || offset + size > buffer.size())
		return false;

	*blob = buffer.mid(offset, size);
	offset += size;
	while ((offset % 4) != 0 && offset < buffer.size())
		++offset;
	return true;
}

QString BlobToText(const QByteArray &blob)
{
	QString text = QStringLiteral("0x");
	for (unsigned char byte : blob)
		text += QStringLiteral("%1").arg(byte, 2, 16, QLatin1Char('0')).toUpper();
	return text;
}

bool TextToBlob(const QString &text, QByteArray *blob, QString *error)
{
	if (!blob)
		return false;

	QString normalized = text.trimmed();
	if (normalized.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
		normalized.remove(0, 2);
	normalized.remove(' ');
	if (normalized.isEmpty()) {
		blob->clear();
		return true;
	}
	if ((normalized.size() % 2) != 0) {
		if (error)
			*error = QStringLiteral("OSC blob text must contain an even number of hex characters");
		return false;
	}

	QByteArray bytes;
	bytes.reserve(normalized.size() / 2);
	for (int index = 0; index < normalized.size(); index += 2) {
		bool ok = false;
		const int value = normalized.mid(index, 2).toInt(&ok, 16);
		if (!ok) {
			if (error)
				*error = QStringLiteral("OSC blob text contains invalid hex data");
			return false;
		}
		bytes.append(char(value & 0xff));
	}

	*blob = bytes;
	return true;
}

QJsonValue ArgumentToJsonValue(const SwitchOscArgument &argument)
{
	switch (argument.type) {
	case SwitchOscArgumentType::Int:
		return QJsonValue(argument.value.toInt());
	case SwitchOscArgumentType::Float: {
		QJsonObject object;
		object.insert(QStringLiteral("type"), QStringLiteral("float"));
		object.insert(QStringLiteral("value"), argument.value.toDouble());
		return object;
	}
	case SwitchOscArgumentType::String:
		return QJsonValue(argument.value.toString());
	case SwitchOscArgumentType::Blob: {
		QJsonObject object;
		object.insert(QStringLiteral("type"), QStringLiteral("blob"));
		object.insert(QStringLiteral("value"), argument.value.toString());
		return object;
	}
	case SwitchOscArgumentType::True:
		return QJsonValue(true);
	case SwitchOscArgumentType::False:
		return QJsonValue(false);
	case SwitchOscArgumentType::Null:
		return QJsonValue(QJsonValue::Null);
	case SwitchOscArgumentType::Infinity: {
		QJsonObject object;
		object.insert(QStringLiteral("type"), QStringLiteral("infinity"));
		return object;
	}
	}
	return QJsonValue(argument.value.toString());
}

QString SummaryValueForArgument(const SwitchOscArgument &argument)
{
	switch (argument.type) {
	case SwitchOscArgumentType::Int:
		return QString::number(argument.value.toInt());
	case SwitchOscArgumentType::Float:
		return QString::number(argument.value.toDouble(), 'f', 3);
	case SwitchOscArgumentType::String:
		return QStringLiteral("\"%1\"").arg(argument.value.toString());
	case SwitchOscArgumentType::Blob:
		return argument.value.toString();
	case SwitchOscArgumentType::True:
		return QStringLiteral("true");
	case SwitchOscArgumentType::False:
		return QStringLiteral("false");
	case SwitchOscArgumentType::Null:
		return QStringLiteral("null");
	case SwitchOscArgumentType::Infinity:
		return QStringLiteral("infinity");
	}
	return argument.value.toString();
}

bool JsonValueToArgument(const QJsonValue &value, SwitchOscArgument *argument, QString *error)
{
	if (!argument)
		return false;

	if (value.isBool()) {
		argument->type = value.toBool() ? SwitchOscArgumentType::True : SwitchOscArgumentType::False;
		argument->value.clear();
		return true;
	}
	if (value.isNull()) {
		argument->type = SwitchOscArgumentType::Null;
		argument->value.clear();
		return true;
	}
	if (value.isString()) {
		argument->type = SwitchOscArgumentType::String;
		argument->value = value.toString();
		return true;
	}
	if (value.isDouble()) {
		const double number = value.toDouble();
		if (std::isfinite(number) && std::llround(number) == number &&
		    number >= std::numeric_limits<qint32>::min() && number <= std::numeric_limits<qint32>::max()) {
			argument->type = SwitchOscArgumentType::Int;
			argument->value = int(number);
		} else {
			argument->type = SwitchOscArgumentType::Float;
			argument->value = number;
		}
		return true;
	}
	if (!value.isObject()) {
		if (error)
			*error = QStringLiteral("OSC arguments must be JSON primitives or typed JSON objects");
		return false;
	}

	const QJsonObject object = value.toObject();
	const QString type = object.value(QStringLiteral("type")).toString().trimmed().toLower();
	if (type.isEmpty()) {
		if (error)
			*error = QStringLiteral("OSC typed arguments require a type field");
		return false;
	}

	if (type == QStringLiteral("int") || type == QStringLiteral("integer")) {
		argument->type = SwitchOscArgumentType::Int;
		argument->value = object.value(QStringLiteral("value")).toInt();
		return true;
	}
	if (type == QStringLiteral("float") || type == QStringLiteral("double")) {
		argument->type = SwitchOscArgumentType::Float;
		argument->value = object.value(QStringLiteral("value")).toDouble();
		return true;
	}
	if (type == QStringLiteral("string")) {
		argument->type = SwitchOscArgumentType::String;
		argument->value = object.value(QStringLiteral("value")).toString();
		return true;
	}
	if (type == QStringLiteral("blob")) {
		argument->type = SwitchOscArgumentType::Blob;
		argument->value = object.value(QStringLiteral("value")).toString();
		return true;
	}
	if (type == QStringLiteral("true")) {
		argument->type = SwitchOscArgumentType::True;
		argument->value.clear();
		return true;
	}
	if (type == QStringLiteral("false")) {
		argument->type = SwitchOscArgumentType::False;
		argument->value.clear();
		return true;
	}
	if (type == QStringLiteral("null") || type == QStringLiteral("nil")) {
		argument->type = SwitchOscArgumentType::Null;
		argument->value.clear();
		return true;
	}
	if (type == QStringLiteral("infinity")) {
		argument->type = SwitchOscArgumentType::Infinity;
		argument->value.clear();
		return true;
	}

	if (error)
		*error = QStringLiteral("Unsupported OSC argument type %1").arg(type);
	return false;
}

QString OscPatternToRegex(const QString &pattern)
{
	QString regex = QStringLiteral("^");
	for (int index = 0; index < pattern.size(); ++index) {
		const QChar ch = pattern.at(index);
		if (ch == QLatin1Char('*')) {
			regex += QStringLiteral(".*");
			continue;
		}
		if (ch == QLatin1Char('?')) {
			regex += QLatin1Char('.');
			continue;
		}
		if (ch == QLatin1Char('{')) {
			const int close = pattern.indexOf(QLatin1Char('}'), index + 1);
			if (close > index) {
				const QStringList options =
					pattern.mid(index + 1, close - index - 1).split(QLatin1Char(','), Qt::KeepEmptyParts);
				QStringList escaped;
				for (const auto &option : options)
					escaped.push_back(QRegularExpression::escape(option));
				regex += QStringLiteral("(?:%1)").arg(escaped.join(QLatin1Char('|')));
				index = close;
				continue;
			}
		}
		if (ch == QLatin1Char('[')) {
			const int close = pattern.indexOf(QLatin1Char(']'), index + 1);
			if (close > index) {
				QString charClass = pattern.mid(index + 1, close - index - 1);
				if (charClass.startsWith(QLatin1Char('!')))
					charClass.replace(0, 1, QStringLiteral("^"));
				charClass.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
				regex += QStringLiteral("[%1]").arg(charClass);
				index = close;
				continue;
			}
		}
		regex += QRegularExpression::escape(QString(ch));
	}
	regex += QStringLiteral("$");
	return regex;
}
} // namespace

QString SwitchOscArgumentTypeName(SwitchOscArgumentType type)
{
	switch (type) {
	case SwitchOscArgumentType::Int:
		return QStringLiteral("int");
	case SwitchOscArgumentType::Float:
		return QStringLiteral("float");
	case SwitchOscArgumentType::String:
		return QStringLiteral("string");
	case SwitchOscArgumentType::Blob:
		return QStringLiteral("blob");
	case SwitchOscArgumentType::True:
		return QStringLiteral("true");
	case SwitchOscArgumentType::False:
		return QStringLiteral("false");
	case SwitchOscArgumentType::Null:
		return QStringLiteral("null");
	case SwitchOscArgumentType::Infinity:
		return QStringLiteral("infinity");
	}
	return QStringLiteral("string");
}

QString SwitchOscArgumentsToConfigString(const QVector<SwitchOscArgument> &arguments)
{
	QJsonArray array;
	for (const auto &argument : arguments)
		array.push_back(ArgumentToJsonValue(argument));
	return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

QString SwitchOscArgumentsToSummaryString(const QVector<SwitchOscArgument> &arguments)
{
	QStringList values;
	values.reserve(arguments.size());
	for (const auto &argument : arguments)
		values.push_back(SummaryValueForArgument(argument));
	return QStringLiteral("[%1]").arg(values.join(QStringLiteral(", ")));
}

QString SwitchOscMessageSummary(const SwitchOscMessage &message)
{
	return QStringLiteral("%1 %2").arg(message.address, SwitchOscArgumentsToSummaryString(message.arguments));
}

bool SwitchParseOscArguments(const QString &input, QVector<SwitchOscArgument> *arguments, QString *error)
{
	if (!arguments)
		return false;

	arguments->clear();
	const QString trimmed = input.trimmed();
	if (trimmed.isEmpty())
		return true;

	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(trimmed.toUtf8(), &parseError);
	if (parseError.error != QJsonParseError::NoError) {
		if (error)
			*error = QStringLiteral("OSC arguments must be valid JSON: %1").arg(parseError.errorString());
		return false;
	}

	QJsonArray values;
	if (document.isArray()) {
		values = document.array();
	} else if (document.isObject()) {
		values.push_back(document.object());
	} else {
		if (error)
			*error = QStringLiteral("OSC arguments must be a JSON array or typed argument object");
		return false;
	}

	for (const auto value : values) {
		SwitchOscArgument argument;
		if (!JsonValueToArgument(value, &argument, error))
			return false;
		arguments->push_back(argument);
	}
	return true;
}

bool SwitchOscArgumentsEqual(const QVector<SwitchOscArgument> &lhs, const QVector<SwitchOscArgument> &rhs)
{
	if (lhs.size() != rhs.size())
		return false;

	for (int index = 0; index < lhs.size(); ++index) {
		if (lhs[index].type != rhs[index].type)
			return false;
		if (lhs[index].value != rhs[index].value)
			return false;
	}
	return true;
}

bool SwitchBuildOscPacket(const SwitchOscMessage &message, QByteArray *packet, QString *error)
{
	if (!packet)
		return false;

	const QString address = message.address.trimmed();
	if (address.isEmpty() || !address.startsWith(QLatin1Char('/'))) {
		if (error)
			*error = QStringLiteral("OSC address must start with '/'");
		return false;
	}

	QByteArray buffer;
	AppendPaddedString(buffer, address);

	QString typeTags = QStringLiteral(",");
	for (const auto &argument : message.arguments)
		typeTags += TypeTagForArgument(argument);
	AppendPaddedString(buffer, typeTags);

	for (const auto &argument : message.arguments) {
		switch (argument.type) {
		case SwitchOscArgumentType::Int:
			AppendInt32(buffer, argument.value.toInt());
			break;
		case SwitchOscArgumentType::Float:
			AppendFloat32(buffer, float(argument.value.toDouble()));
			break;
		case SwitchOscArgumentType::String:
			AppendPaddedString(buffer, argument.value.toString());
			break;
		case SwitchOscArgumentType::Blob: {
			QByteArray blob;
			if (!TextToBlob(argument.value.toString(), &blob, error))
				return false;
			AppendBlob(buffer, blob);
			break;
		}
		case SwitchOscArgumentType::True:
		case SwitchOscArgumentType::False:
		case SwitchOscArgumentType::Null:
		case SwitchOscArgumentType::Infinity:
			break;
		}
	}

	*packet = buffer;
	return true;
}

bool SwitchParseOscPacket(const QByteArray &packet, SwitchOscMessage *message, QString *error)
{
	if (!message)
		return false;
	message->address.clear();
	message->arguments.clear();

	int offset = 0;
	QString address;
	if (!ReadPaddedString(packet, offset, &address)) {
		if (error)
			*error = QStringLiteral("OSC packet does not contain a valid address");
		return false;
	}
	if (address == QStringLiteral("#bundle")) {
		if (error)
			*error = QStringLiteral("OSC bundles are not supported in this build");
		return false;
	}
	if (!address.startsWith(QLatin1Char('/'))) {
		if (error)
			*error = QStringLiteral("OSC packet address must start with '/'");
		return false;
	}

	QString typeTags;
	if (!ReadPaddedString(packet, offset, &typeTags) || !typeTags.startsWith(QLatin1Char(','))) {
		if (error)
			*error = QStringLiteral("OSC packet is missing a valid type tag string");
		return false;
	}

	QVector<SwitchOscArgument> arguments;
	for (int index = 1; index < typeTags.size(); ++index) {
		const QChar tag = typeTags.at(index);
		SwitchOscArgument argument;
		switch (tag.toLatin1()) {
		case 'i': {
			qint32 value = 0;
			if (!ReadInt32(packet, offset, &value)) {
				if (error)
					*error = QStringLiteral("OSC packet ended while reading int argument");
				return false;
			}
			argument.type = SwitchOscArgumentType::Int;
			argument.value = int(value);
			break;
		}
		case 'f': {
			float value = 0.0f;
			if (!ReadFloat32(packet, offset, &value)) {
				if (error)
					*error = QStringLiteral("OSC packet ended while reading float argument");
				return false;
			}
			argument.type = SwitchOscArgumentType::Float;
			argument.value = double(value);
			break;
		}
		case 's': {
			QString value;
			if (!ReadPaddedString(packet, offset, &value)) {
				if (error)
					*error = QStringLiteral("OSC packet ended while reading string argument");
				return false;
			}
			argument.type = SwitchOscArgumentType::String;
			argument.value = value;
			break;
		}
		case 'b': {
			QByteArray blob;
			if (!ReadBlob(packet, offset, &blob)) {
				if (error)
					*error = QStringLiteral("OSC packet ended while reading blob argument");
				return false;
			}
			argument.type = SwitchOscArgumentType::Blob;
			argument.value = BlobToText(blob);
			break;
		}
		case 'T':
			argument.type = SwitchOscArgumentType::True;
			break;
		case 'F':
			argument.type = SwitchOscArgumentType::False;
			break;
		case 'N':
			argument.type = SwitchOscArgumentType::Null;
			break;
		case 'I':
			argument.type = SwitchOscArgumentType::Infinity;
			break;
		default:
			if (error)
				*error = QStringLiteral("OSC packet uses unsupported type tag '%1'").arg(tag);
			return false;
		}
		arguments.push_back(argument);
	}

	message->address = address;
	message->arguments = arguments;
	return true;
}

bool SwitchOscAddressPatternMatches(const QString &pattern, const QString &address)
{
	const QString trimmed = pattern.trimmed();
	if (trimmed.isEmpty())
		return true;
	const QRegularExpression regex(OscPatternToRegex(trimmed));
	return regex.isValid() && regex.match(address).hasMatch();
}
