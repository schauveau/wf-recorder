#include "averr.h"

const char* averr(int err)
{
  static char buffer[AV_ERROR_MAX_STRING_SIZE] ;
  av_make_error_string(buffer, AV_ERROR_MAX_STRING_SIZE, err);    
  return buffer;
}
