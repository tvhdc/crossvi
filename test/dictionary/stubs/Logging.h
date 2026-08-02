#pragma once

template <typename... Args>
inline void dictionaryLogStub(Args&&...) {}

#define LOG_DBG(...) dictionaryLogStub(__VA_ARGS__)
#define LOG_INF(...) dictionaryLogStub(__VA_ARGS__)
#define LOG_ERR(...) dictionaryLogStub(__VA_ARGS__)
