#pragma once

#include <QString>
#include <QRegularExpression>
#include <QVersionNumber>

namespace VersionUtils {

inline QVersionNumber parseNumeric(const QString& text)
{
    const QString version = text.trimmed();
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
    qsizetype suffixIndex = 0;
#else
    int suffixIndex = 0;
#endif
    const QVersionNumber parsed = QVersionNumber::fromString(version, &suffixIndex);
    return suffixIndex == version.size() ? parsed : QVersionNumber();
}

inline QVersionNumber parseRelease(QString version)
{
    version = version.trimmed();
    if (version.startsWith('v') || version.startsWith('V')) {
        version.remove(0, 1);
    }
    const int metadata = version.indexOf('+');
    const int prerelease = version.indexOf('-');
    int suffix = metadata;
    if (suffix < 0 || (prerelease >= 0 && prerelease < suffix)) {
        suffix = prerelease;
    }
    if (suffix >= 0) {
        version.truncate(suffix);
    }
    return parseNumeric(version);
}

inline QVersionNumber parseBuild(const QString& version)
{
    const auto base = parseRelease(version);
    static const QRegularExpression revision(QStringLiteral("[+-](\\d+)\\.g[0-9a-fA-F]+"));
    const auto match = revision.match(version);
    if (!match.hasMatch() || base.segmentCount() < 3) {
        return base;
    }
    bool ok = false;
    const int distance = match.captured(1).toInt(&ok);
    auto segments = base.segments();
    if (segments.size() == 3)
        segments.append(0);
    if (!ok || segments.size() != 4 || distance > 65535 - segments[3])
        return QVersionNumber();
    segments[3] += distance;
    return QVersionNumber(segments);
}

// Preserve the existing rule that absent trailing segments are zero.
inline int compare(const QVersionNumber& lhs, const QVersionNumber& rhs)
{
    return QVersionNumber::compare(lhs.normalized(), rhs.normalized());
}

} // namespace VersionUtils
