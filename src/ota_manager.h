#pragma once
#include <Arduino.h>

enum class OtaResult {
    UP_TO_DATE,
    HW_INCOMPATIBLE,
    METADATA_FETCH_FAILED,
    DOWNLOAD_FAILED,
    HASH_MISMATCH,
    UPDATED_REBOOTING,  
    ERROR
};

OtaResult otaCheckAndUpdate();

void otaRunSelfTestAndConfirm();

void otaManualRollback();

String otaGetCurrentVersion();

void otaLogBootInfo();
