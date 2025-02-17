// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef _ST_HPC_PPL_NN_PARAMS_ONNX_CLIP_PARAM_H_
#define _ST_HPC_PPL_NN_PARAMS_ONNX_CLIP_PARAM_H_

#include <limits>
#include "ppl/nn/ir/attr.h"

namespace ppl { namespace nn { namespace onnx {

/** @brief this param is for opset 6 */
struct ClipParam final : public ir::TypedAttr<ClipParam> {
    float min_value = std::numeric_limits<float>::lowest();
    float max_value = std::numeric_limits<float>::max();

    bool operator==(const ClipParam& p) const {
        return (min_value == p.min_value && max_value == p.max_value);
    }
};

}}} // namespace ppl::nn::onnx

#endif
