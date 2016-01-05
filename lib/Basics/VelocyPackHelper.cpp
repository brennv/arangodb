////////////////////////////////////////////////////////////////////////////////
/// @brief velocypack helper functions
///
/// @file
///
/// DISCLAIMER
///
/// Copyright 2015 ArangoDB GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Michael Hackstein
/// @author Copyright 2015, ArangoDB GmbH, Cologne, Germany
////////////////////////////////////////////////////////////////////////////////

#include "Basics/VelocyPackHelper.h"
#include "Basics/Exceptions.h"
#include "Basics/files.h"

using VelocyPackHelper = triagens::basics::VelocyPackHelper;

// -----------------------------------------------------------------------------
// --SECTION--                                            class VelocyPackHelper
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                             public static methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a boolean sub-element, or a default if it is does not exist
////////////////////////////////////////////////////////////////////////////////

bool VelocyPackHelper::getBooleanValue (VPackSlice const& slice,
                                        char const* name,
                                        bool defaultValue) {
  VPackSlice const& sub = slice.get(name);

  if (sub.isBoolean()) {
    return sub.getBool();
  }

  return defaultValue;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a string sub-element, or throws if <name> does not exist
/// or it is not a string 
////////////////////////////////////////////////////////////////////////////////

std::string VelocyPackHelper::checkAndGetStringValue (VPackSlice const& slice,
                                                      char const* name) {
  TRI_ASSERT(slice.isObject());
  VPackSlice const sub = slice.get(name);
  if (! sub.isString()) {
    std::string msg = "The attribute '" + std::string(name)  
      + "' was not found or is not a string.";
    THROW_ARANGO_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER, msg);
  }
  return sub.copyString();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns a string sub-element, or the default value if it does not exist
/// or it is not a string 
////////////////////////////////////////////////////////////////////////////////

std::string VelocyPackHelper::getStringValue (VPackSlice const& slice,
                                             char const* name,
                                             std::string const& defaultValue) {
  TRI_ASSERT(slice.isObject());
  VPackSlice const sub = slice.get(name);
  if (! sub.isString()) {
    return defaultValue;
  }
  return sub.copyString();
}

TRI_json_t* VelocyPackHelper::velocyPackToJson (VPackSlice const& slice) {
  return JsonHelper::fromString(slice.toJson());
}

////////////////////////////////////////////////////////////////////////////////
/// @brief parses a json file to VelocyPack
//////////////////////////////////////////////////////////////////////////////////

std::shared_ptr<VPackBuilder> VelocyPackHelper::velocyPackFromFile (std::string const& path) {
  size_t length;
  char* content = TRI_SlurpFile(TRI_UNKNOWN_MEM_ZONE, path.c_str(), &length);
  if (content != nullptr) {
    // The Parser might THROW
    std::shared_ptr<VPackBuilder> b;
    try {
      auto b = VPackParser::fromJson(reinterpret_cast<uint8_t const*>(content), length);
      TRI_Free(TRI_UNKNOWN_MEM_ZONE, content);
      return b;
    }
    catch (...) {
      TRI_Free(TRI_UNKNOWN_MEM_ZONE, content);
      throw;
    }
  }
  THROW_ARANGO_EXCEPTION(TRI_ERROR_INTERNAL);
}

// -----------------------------------------------------------------------------
// --SECTION--                                                       END-OF-FILE
// -----------------------------------------------------------------------------

// Local Variables:
// mode: outline-minor
// outline-regexp: "/// @brief\\|/// {@inheritDoc}\\|/// @page\\|// --SECTION--\\|/// @\\}"
// End: