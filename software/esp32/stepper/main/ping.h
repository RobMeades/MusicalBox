/*
 * Copyright 2026 Rob Meades
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

 #ifndef _PING_H_
 #define _PING_H_
 
 /** @file
  * @brief The ping API for the stepper motor application.
  */
 
 #ifdef __cplusplus
 extern "C" {
 #endif

 /* ----------------------------------------------------------------
  * COMPILE-TIME MACROS
  * -------------------------------------------------------------- */
 
 /* ----------------------------------------------------------------
  * TYPES
  * -------------------------------------------------------------- */
  
 /* ----------------------------------------------------------------
  * FUNCTIONS
  * -------------------------------------------------------------- */
 
/** Start a ping session, of default type, to the given host name.
 * Networking must have been started.  Pinging will continue in the
 * background and diagnostic prints will be emitted.
 *
 * @param hostname  the host name, a null-terminated string.
 * @return          ESP_OK on successful start, else a negative value
 *                  from esp_err_t.
 */
esp_err_t ping_start(const char *hostname);

 #ifdef __cplusplus
 }
 #endif
 
 /** @}*/
 
 #endif // _PING_H_
 
 // End of file