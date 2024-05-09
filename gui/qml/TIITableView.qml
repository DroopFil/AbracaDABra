/*
 * This file is part of the AbracaDABra project
 *
 * MIT License
 *
 * Copyright (c) 2019-2024 Petr Kopecký <xkejpi (at) gmail (dot) com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

import QtQuick
import QtQuick.Controls

Item {
    id: tiiTableItem
    FontMetrics {
        id: fontMetrics
    }
    HoverHandler {
        id: tiiTableItemHoverHandler
    }
    opacity: tiiTableItemHoverHandler.hovered ? 1.0 : 0.75

    readonly property int rowHeight: fontMetrics.font.pointSize + 10

    width: 100
    height: Math.min((tiiTableItem.rowHeight * tiiTableView.model.rowCount) + horizontalHeader.height, parent.height-20)
    visible: tiiTableView.model.rowCount > 0

    property var columnWidths: []
    Component.onCompleted: {
        for (var n = 0; n < tiiTableView.model.columnCount(); ++n) {
            columnWidths[n] = Math.ceil(fontMetrics.boundingRect(tiiTableView.model.headerData(n, Qt.Horizontal, Qt.DisplayRole)).width + (n < 2 ? 15 : 25));
        }
        columnWidths[0] = Math.max(columnWidths[0], columnWidths[1]);
        columnWidths[1] = columnWidths[0];
        var w = 0;
        for (n = 0; n < tiiTableView.model.columnCount(); ++n) {
            w += columnWidths[n];
        }
        width = w;
        tiiTableView.forceLayout();
    }

    HorizontalHeaderView {
        id: horizontalHeader
        anchors.left: tiiTableView.left
        anchors.top: parent.top
        syncView: tiiTableView
        clip: true
        property int sortIndicatorColumn: 2
        property int sortIndicatorOrder: Qt.DescendingOrder
        Component.onCompleted: {
            tiiTableView.model.sort(sortIndicatorColumn, sortIndicatorOrder);
        }
        delegate: Rectangle {
            color: "gainsboro"
            implicitWidth: 10
            implicitHeight: tiiTableItem.rowHeight
            Text {
                property string indicator: (index === horizontalHeader.sortIndicatorColumn)
                                           ? " " + (horizontalHeader.sortIndicatorOrder === Qt.AscendingOrder ? String.fromCodePoint(0x23F6) : String.fromCodePoint(0x23F7))
                                           : "";
                anchors.centerIn: parent
                text: display
                        ? display + indicator
                        : ""
            }
            MouseArea {
                anchors.fill: parent
                onClicked:{
                    if (index === horizontalHeader.sortIndicatorColumn) {
                        // change order
                        horizontalHeader.sortIndicatorOrder = (horizontalHeader.sortIndicatorOrder === Qt.AscendingOrder ? Qt.DescendingOrder : Qt.AscendingOrder);
                    }
                    else {
                        horizontalHeader.sortIndicatorOrder = Qt.AscendingOrder;
                        horizontalHeader.sortIndicatorColumn = index;
                    }
                    tiiTableView.model.sort(index, horizontalHeader.sortIndicatorOrder);
                }
            }
        }
    }
    TableView {
        id: tiiTableView
        anchors.left: parent.left
        anchors.top: horizontalHeader.bottom
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: tiiTableSorted
        selectionModel: tiiTableSelectionModel
        delegate: Rectangle {
            implicitWidth: 10
            implicitHeight: tiiTableItem.rowHeight
            required property bool current
            required property bool selected
            color: selected ? palette.highlight : palette.base
            Text {
                anchors.centerIn: parent
                text: display ? display : ""
                color: selected ? palette.highlightedText : palette.text
            }

            MouseArea {
                anchors.fill: parent
                onClicked: {
                    tiiTableView.selectionModel.select(tiiTableView.model.index(row, 0), ItemSelectionModel.ClearAndSelect | ItemSelectionModel.Current | ItemSelectionModel.Rows);

                }
            }
        }

        columnWidthProvider: function (column) { return tiiTableItem.columnWidths[column] }
    }
}
