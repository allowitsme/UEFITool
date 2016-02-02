/* fitparser.cpp

Copyright (c) 2015, Nikolaj Schlej. All rights reserved.
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHWARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*/
#include "fitparser.h"
#include "types.h"
#include "treemodel.h"


FitParser::FitParser(TreeModel* treeModel, QObject *parent)
    : QObject(parent), model(treeModel)
{
}

FitParser::~FitParser()
{
}

void FitParser::msg(const QString & message, const QModelIndex & index)
{
    messagesVector.push_back(QPair<QString, QModelIndex>(message, index));
}

QVector<QPair<QString, QModelIndex> > FitParser::getMessages() const
{
    return messagesVector;
}

void FitParser::clearMessages()
{
    messagesVector.clear();
}

STATUS FitParser::parse(const QModelIndex & index, const QModelIndex & lastVtfIndex)
{
    // Check sanity
    if (!index.isValid() || !lastVtfIndex.isValid())
        return EFI_INVALID_PARAMETER;

    // Store lastVtfIndex
    lastVtf = lastVtfIndex;

    // Search for FIT
    QModelIndex fitIndex;
    UINT32 fitOffset;
    STATUS result = findFitRecursive(index, fitIndex, fitOffset);
    if (result)
        return result;

    // FIT not found
    if (!fitIndex.isValid())
        return ERR_SUCCESS;
    
    // Explicitly set the item as fixed
    model->setFixed(index, true);

    // Special case of FIT header
    const FIT_ENTRY* fitHeader = (const FIT_ENTRY*)(model->body(fitIndex).constData() + fitOffset);
    
    // Check FIT checksum, if present
    UINT32 fitSize = (fitHeader->Size & 0xFFFFFF) << 4;
    if (fitHeader->Type & 0x80) {
        // Calculate FIT entry checksum
        QByteArray tempFIT = model->body(fitIndex).mid(fitOffset, fitSize);
        FIT_ENTRY* tempFitHeader = (FIT_ENTRY*)tempFIT.data();
        tempFitHeader->Checksum = 0;
        UINT8 calculated = calculateChecksum8((const UINT8*)tempFitHeader, fitSize);
        if (calculated != fitHeader->Checksum) {
            msg(tr("Invalid FIT table checksum %1h, should be %2h").hexarg2(fitHeader->Checksum, 2).hexarg2(calculated, 2), fitIndex);
        }
    }

    // Check fit header type
    if ((fitHeader->Type & 0x7F) != FIT_TYPE_HEADER) {
        msg(tr("Invalid FIT header type"), fitIndex);
    }

    // Add FIT header to fitTable
    QVector<QString> currentStrings;
    currentStrings += tr("_FIT_   ");
    currentStrings += tr("%1").hexarg2(fitSize, 8);
    currentStrings += tr("%1").hexarg2(fitHeader->Version, 4);
    currentStrings += fitEntryTypeToQString(fitHeader->Type);
    currentStrings += tr("%1").hexarg2(fitHeader->Checksum, 2);
    fitTable.append(currentStrings);

    // Process all other entries
    bool msgModifiedImageMayNotWork = false;
    for (UINT32 i = 1; i < fitHeader->Size; i++) {
        currentStrings.clear();
        const FIT_ENTRY* currentEntry = fitHeader + i;

        // Check entry type
        switch (currentEntry->Type & 0x7F) {
        case FIT_TYPE_HEADER:
            msg(tr("Second FIT header found, the table is damaged"), fitIndex);
            break;

        case FIT_TYPE_EMPTY:
        case FIT_TYPE_MICROCODE:
            break;

        case FIT_TYPE_BIOS_AC_MODULE:
        case FIT_TYPE_BIOS_INIT_MODULE:
        case FIT_TYPE_TPM_POLICY:
        case FIT_TYPE_BIOS_POLICY_DATA:
        case FIT_TYPE_TXT_CONF_POLICY:
        case FIT_TYPE_AC_KEY_MANIFEST:
        case FIT_TYPE_AC_BOOT_POLICY:
        default:
            msgModifiedImageMayNotWork = true;
            break;
        }

        // Add entry to fitTable
        currentStrings += tr("%1").hexarg2(currentEntry->Address, 16);
        currentStrings += tr("%1").hexarg2(currentEntry->Size, 8);
        currentStrings += tr("%1").hexarg2(currentEntry->Version, 4);
        currentStrings += fitEntryTypeToQString(currentEntry->Type);
        currentStrings += tr("%1").hexarg2(currentEntry->Checksum, 2);
        fitTable.append(currentStrings);
    }

    if (msgModifiedImageMayNotWork)
        msg(tr("Opened image may not work after any modification"));

    return ERR_SUCCESS;
}

QString FitParser::fitEntryTypeToQString(UINT8 type)
{
    switch (type & 0x7F) {
    case FIT_TYPE_HEADER:           return tr("Header");
    case FIT_TYPE_MICROCODE:        return tr("Microcode");
    case FIT_TYPE_BIOS_AC_MODULE:   return tr("BIOS ACM");
    case FIT_TYPE_BIOS_INIT_MODULE: return tr("BIOS Init");
    case FIT_TYPE_TPM_POLICY:       return tr("TPM Policy");
    case FIT_TYPE_BIOS_POLICY_DATA: return tr("BIOS Policy Data");
    case FIT_TYPE_TXT_CONF_POLICY:  return tr("TXT Configuration Policy");
    case FIT_TYPE_AC_KEY_MANIFEST:  return tr("BootGuard Key Manifest");
    case FIT_TYPE_AC_BOOT_POLICY:   return tr("BootGuard Boot Policy");
    case FIT_TYPE_EMPTY:            return tr("Empty");
    default:                        return tr("Unknown");
    }
}

STATUS FitParser::findFitRecursive(const QModelIndex & index, QModelIndex & found, UINT32 & fitOffset)
{
    // Sanity check
    if (!index.isValid())
        return EFI_SUCCESS;

    // Process child items
    for (int i = 0; i < model->rowCount(index); i++) {
        findFitRecursive(index.child(i, 0), found, fitOffset);
        if (found.isValid())
            return EFI_SUCCESS;
    }

    // Get parsing data for the current item
    PARSING_DATA pdata = parsingDataFromQModelIndex(index);

    // Check for all FIT signatures in item's body
    for (INT32 offset = model->body(index).indexOf(FIT_SIGNATURE); 
         offset >= 0; 
         offset = model->body(index).indexOf(FIT_SIGNATURE, offset + 1)) {
        // FIT candidate found, calculate it's physical address
        UINT32 fitAddress = pdata.address + model->header(index).size() + (UINT32)offset;
            
        // Check FIT address to be in the last VTF
        QByteArray lastVtfBody = model->body(lastVtf);
        if (*(const UINT32*)(lastVtfBody.constData() + lastVtfBody.size() - FIT_POINTER_OFFSET) == fitAddress) {
            found = index;
            fitOffset = offset;
            msg(tr("Real FIT table found at physical address %1h").hexarg(fitAddress), found);
            return ERR_SUCCESS;
        }
        else if (model->rowCount(index) == 0) // Show messages only to leaf items
            msg(tr("FIT table candidate found, but not referenced from the last VTF"), index);
    }

    return ERR_SUCCESS;
}