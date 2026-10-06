/****************************************************************************
**
** Copyright (C) 2007 Trolltech ASA. All rights reserved.
**
** This file is part of the Qt Concurrent project on Trolltech Labs.
**
** This file may be used under the terms of the GNU General Public
** License version 2.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of
** this file.  Please review the following information to ensure GNU
** General Public Licensing requirements will be met:
** http://www.trolltech.com/products/qt/licensing.html
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#ifndef MODELTEST_H
#define MODELTEST_H

// TST01: the Qt4-era vendored ModelTest implementation was superseded
// by QAbstractItemModelTester (shipped in QtTest since Qt 5.11).  This
// shim keeps the original class name and constructor signature so the
// call sites don't change.
#include <QtTest/QAbstractItemModelTester>

class ModelTest : public QAbstractItemModelTester
{
public:
    explicit ModelTest(QAbstractItemModel *model, QObject *parent = nullptr)
        : QAbstractItemModelTester(model, QAbstractItemModelTester::FailureReportingMode::Fatal, parent)
    {
    }
};

#endif
