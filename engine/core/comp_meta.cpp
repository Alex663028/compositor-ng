// AI生成
/*
 * comp_meta.cpp — comp_core.h 中三类“元信息”查询的实现。
 */
#include "compositor/comp_core.h"

extern "C" const char* comp_version(void) {
  return "1.0.0";
}

extern "C" const char* comp_result_name(comp_result result) {
  switch (result) {
    case COMP_OK:                   return "COMP_OK";
    case COMP_ERR_INVALID_ARGUMENT: return "COMP_ERR_INVALID_ARGUMENT";
    case COMP_ERR_OUT_OF_MEMORY:    return "COMP_ERR_OUT_OF_MEMORY";
    case COMP_ERR_IO:               return "COMP_ERR_IO";
    case COMP_ERR_UNSUPPORTED:      return "COMP_ERR_UNSUPPORTED";
    case COMP_ERR_STATE:            return "COMP_ERR_STATE";
    case COMP_ERR_BACKEND:          return "COMP_ERR_BACKEND";
    case COMP_ERR_NOT_FOUND:        return "COMP_ERR_NOT_FOUND";
    case COMP_ERR_ALREADY_EXISTS:   return "COMP_ERR_ALREADY_EXISTS";
    case COMP_ERR_DECODE:           return "COMP_ERR_DECODE";
    case COMP_ERR_CANCELLED:        return "COMP_ERR_CANCELLED";
    case COMP_ERR_INTERNAL:         return "COMP_ERR_INTERNAL";
    default:                        return "COMP_ERR_UNKNOWN";
  }
}

extern "C" const char* comp_blend_mode_name(comp_blend_mode mode) {
  switch (mode) {
    case COMP_BLEND_NORMAL:       return "NORMAL";
    case COMP_BLEND_DARKEN:       return "DARKEN";
    case COMP_BLEND_MULTIPLY:     return "MULTIPLY";
    case COMP_BLEND_COLOR_BURN:   return "COLOR_BURN";
    case COMP_BLEND_LINEAR_BURN:  return "LINEAR_BURN";
    case COMP_BLEND_LIGHTEN:      return "LIGHTEN";
    case COMP_BLEND_SCREEN:       return "SCREEN";
    case COMP_BLEND_COLOR_DODGE:  return "COLOR_DODGE";
    case COMP_BLEND_LINEAR_DODGE: return "LINEAR_DODGE";
    case COMP_BLEND_OVERLAY:      return "OVERLAY";
    case COMP_BLEND_SOFT_LIGHT:   return "SOFT_LIGHT";
    case COMP_BLEND_HARD_LIGHT:   return "HARD_LIGHT";
    case COMP_BLEND_VIVID_LIGHT:  return "VIVID_LIGHT";
    case COMP_BLEND_LINEAR_LIGHT: return "LINEAR_LIGHT";
    case COMP_BLEND_PIN_LIGHT:    return "PIN_LIGHT";
    case COMP_BLEND_HARD_MIX:     return "HARD_MIX";
    case COMP_BLEND_DIFFERENCE:   return "DIFFERENCE";
    case COMP_BLEND_EXCLUSION:    return "EXCLUSION";
    case COMP_BLEND_SUBTRACT:     return "SUBTRACT";
    case COMP_BLEND_DIVIDE:       return "DIVIDE";
    case COMP_BLEND_HUE:          return "HUE";
    case COMP_BLEND_SATURATION:   return "SATURATION";
    case COMP_BLEND_COLOR:        return "COLOR";
    case COMP_BLEND_LUMINOSITY:   return "LUMINOSITY";
    default:                      return "UNKNOWN";
  }
}
