# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Open/R's generated Thrift archives use generic basenames shared by other
# dependencies, so resolve them only from the selected Open/R install root.
function(bgp_find_openr_library OUTPUT_VARIABLE LIBRARY_NAME)
  find_library(
    ${OUTPUT_VARIABLE}
    NAMES ${LIBRARY_NAME}
    PATHS "${OPENR_LIBRARY_DIR}"
    NO_DEFAULT_PATH
    REQUIRED
  )
  set(${OUTPUT_VARIABLE} "${${OUTPUT_VARIABLE}}" PARENT_SCOPE)
endfunction()
