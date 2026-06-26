/*
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "DataColumnDelegate.h"
#include "BaseTraceViewModel.h"
#include "core/ThemeManager.h"

#include <QPainter>
#include <QApplication>

DataColumnDelegate::DataColumnDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void DataColumnDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    uint64_t changedMask = index.data(BaseTraceViewModel::ChangedBytesRole).value<uint64_t>();
    if (changedMask == 0) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyleOptionViewItem opt = option;
    initStyleOption(&opt, index);

    const QString text = opt.text;
    opt.text.clear();

    QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

    const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
    const int textMargin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, opt.widget) + 1;
    const QFontMetrics fm(opt.font);

    const bool selected = opt.state & QStyle::State_Selected;
    const QColor normalColor = opt.palette.color(
        selected ? QPalette::Active : QPalette::Normal,
        selected ? QPalette::HighlightedText : QPalette::Text);
    const bool isDark = ThemeManager::instance().isDarkMode();
    const QColor changedColor = isDark ? QColor(210, 120, 0) : QColor(180, 90, 0);

    // Error color (from ForegroundRole) takes priority over changed-byte color.
    const QVariant fgVariant = index.data(Qt::ForegroundRole);
    const bool hasErrorColor = fgVariant.isValid();
    const QColor errorColor  = hasErrorColor ? fgVariant.value<QColor>() : QColor();

    constexpr int drawFlags = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine;

    painter->save();
    painter->setFont(opt.font);
    painter->setClipRect(textRect);

    // Text format: "AB CD EF ..." — split on spaces to get one token per byte
    const QStringList tokens = text.split(QLatin1Char(' '));
    int x = textRect.left() + textMargin;
    for (int i = 0; i < tokens.size(); ++i) {
        const bool changed = !hasErrorColor && (i < 64) && ((changedMask >> i) & 1ULL);
        painter->setPen(hasErrorColor ? errorColor : (changed ? changedColor : normalColor));

        const int tokenWidth = fm.horizontalAdvance(tokens.at(i));
        painter->drawText(QRect(x, textRect.top(), tokenWidth, textRect.height()),
                          drawFlags, tokens.at(i));
        x += tokenWidth;

        if (i < tokens.size() - 1) {
            const int spaceWidth = fm.horizontalAdvance(QLatin1Char(' '));
            painter->setPen(hasErrorColor ? errorColor : normalColor);
            painter->drawText(QRect(x, textRect.top(), spaceWidth, textRect.height()),
                              drawFlags, QStringLiteral(" "));
            x += spaceWidth;
        }
    }

    painter->restore();
}
