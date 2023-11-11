#ifndef CORE_CONFIGURATION_PREPROCESSOR_H
#define CORE_CONFIGURATION_PREPROCESSOR_H

#define CONCATENATE(a, b) CONCATENATE_ARG((a, b))

#define CONCATENATE_ARG(parameters) CONCATENATE_HELPER##parameters
#define CONCATENATE_HELPER(a, b) a##b

// ======================================================================================

#define STRINGIZE(text) STRINGIZE_ARG((text))

#define STRINGIZE_ARG(parameter) STRINGIZE_HELPER##parameter
#define STRINGIZE_HELPER(text) #text

// ======================================================================================

#define W_STRINGIZE(text) W_STRINGIZE_ARG((text))

#define W_STRINGIZE_ARG(parameter) W_STRINGIZE_HELPER1##parameter
#define W_STRINGIZE_HELPER1(text) W_STRINGIZE_HELPER2(#text)
#define W_STRINGIZE_HELPER2(string) L##string

// ======================================================================================

#define U8_STRINGIZE(text) U8_STRINGIZE_ARG((text))

#define U8_STRINGIZE_ARG(parameter) U8_STRINGIZE_HELPER1##parameter
#define U8_STRINGIZE_HELPER1(text) U8_STRINGIZE_HELPER2(#text)
#define U8_STRINGIZE_HELPER2(string) u8##string

// ======================================================================================

#define U16_STRINGIZE(text) U16_STRINGIZE_ARG((text))

#define U16_STRINGIZE_ARG(parameter) U16_STRINGIZE_HELPER1##parameter
#define U16_STRINGIZE_HELPER1(text) U16_STRINGIZE_HELPER2(#text)
#define U16_STRINGIZE_HELPER2(string) u##string

// ======================================================================================

#define U32_STRINGIZE(text) U32_STRINGIZE_ARG((text))

#define U32_STRINGIZE_ARG(parameter) U32_STRINGIZE_HELPER1##parameter
#define U32_STRINGIZE_HELPER1(text) U32_STRINGIZE_HELPER2(#text)
#define U32_STRINGIZE_HELPER2(string) U##string

#endif
