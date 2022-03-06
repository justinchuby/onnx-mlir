/*
 * SPDX-License-Identifier: Apache-2.0
 */

//====------ ZHighToZLow.cpp - ZHigh dialect to ZLow lowering -------------===//
//
// Copyright 2019-2020 The IBM Research Authors.
//
// =============================================================================
//
// This file implements the lowering of ZHigh operations to ZLow operations.
//
//===----------------------------------------------------------------------===//

#include <map>
#include <stdio.h>

#include "mlir/Dialect/Affine/Analysis/Utils.h"
#include "mlir/Dialect/Affine/Utils.h"
#include "mlir/Dialect/Shape/IR/Shape.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/DialectConversion.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Sequence.h"

#include "Conversion/ZHighToZLow/ZHighToZLow.hpp"
#include "Dialect/ZHigh/ZHighHelper.hpp"
#include "Dialect/ZHigh/ZHighOps.hpp"
#include "Dialect/ZHigh/ZHighShapeHelper.hpp"
#include "Dialect/ZLow/ZLowOps.hpp"
#include "Pass/DLCPasses.hpp"
#include "src/Conversion/ONNXToKrnl/ONNXToKrnlCommon.hpp"
#include "src/Dialect/Krnl/KrnlOps.hpp"
//#include "third_party/onnx-mlir/src/Conversion/ONNXToKrnl/RNN/RNNBase.hpp"
#include "src/Conversion/ONNXToKrnl/RNN/RNNBase.hpp"

using namespace mlir;

using llvm::SmallMapVector;

// A global variable to indicate whether this pass will emit dealloc for
// allocated memrefs or not.
extern bool ONNXToKrnl_gEmitDealloc;

//===----------------------------------------------------------------------===//
// Helper function of Zhigh to Zlow lowering
// Insert an allocation and deallocation for the given dimensions and layout.
// By default, set aligment to 4K.
//===----------------------------------------------------------------------===//

Value insertAllocAndDeallocZMemRefByDim(ArrayRef<IndexExpr> dims,
    ZTensorEncodingAttr::DataLayout layout, Operation *op,
    PatternRewriter &rewriter, int64_t alignment = gAlignment) {
  // Construct a MemRefType for the given dimensions and element type.
  SmallVector<int64_t, 4> shape;
  for (IndexExpr d : dims)
    shape.emplace_back((d.isLiteral() ? d.getLiteral() : -1));
  RankedTensorType tensorType =
      RankedTensorType::get(shape, rewriter.getF32Type(),
          ZTensorEncodingAttr::get(op->getContext(), layout));
  ZMemRefType zMemRefType = convertZTensorToMemRefType(rewriter, tensorType);

  // Insert alloc and dealloc.
  Value alloc =
      insertAllocAndDeallocZMemRef(zMemRefType, dims, op, rewriter, alignment);

  return alloc;
}

//===----------------------------------------------------------------------===//
// Helper function of Zhigh to Zlow lowering
// Insert an allocation and deallocation for the given ZMemRefType.
// By default, set aligment to 4K.
//===----------------------------------------------------------------------===//

Value insertAllocAndDeallocZMemRef(ZMemRefType zType, ArrayRef<IndexExpr> dims,
    Operation *op, PatternRewriter &rewriter, int64_t alignment = gAlignment) {

  Location loc = op->getLoc();
  MemRefType resType = zType.value;

  // Insert alloc and dealloc.
  Value alloc = insertAllocAndDeallocSimple(rewriter, op, resType, loc,
      SmallVector<IndexExpr>(dims.begin(), dims.end()),
      /*insertDealloc*/ ONNXToKrnl_gEmitDealloc, alignment);

  // Store the buffer's layout. Otherwise, we lost the layout.
  storeLayout(alloc, zType.layout);
  return alloc;
}

/// Insert allocation and deallocation for a 4K-aligned buffer of type
/// <sizexi8> to be used as work_area in LSTM/GRU, where size is computed as
/// follows.
//
/// Enough contiguous storage for the following stickified ztensors
/// dimensions:
///     Fused ztensor
///         dim1 = hidden_state_size
///         dim2 = batch
///         dim3 = 1
///         dim4 = numOfGates * timestep
///
///     Bias Add ztensor
///         dim1 = hidden_state_size
///         dim2 = batch
///         dim3 = 1
///         dim4 = numOfGates
///
///     C output ztensor
///         dim1 = hidden_state_size
///         dim2 = batch
///         dim3 = 1
///         dim4 = 2
///
/// For bidirectional, twice the amount of contiguous storage is required.
///
/// The start of the buffer must be 4k aligned. The work area size is
/// computed as follows.
///   zdnn_tensor_desc desc;
///   desc.dim4 = (numOfGates * timestep) + numOfGates + 2;
///   desc.dim3 = 1;
///   desc.dim2 = batch;
///   desc.dim1 = hidden_state_size;
///   uint64_t work_area_size = zdnn_getsize_ztensor(&desc);
///
/// It is unfolded to:
/// work_area_size in bytes = dim4 * dim3 *
///                           CEIL(dim2, AIU_STICKS_PER_PAGE) *
///                           CEIL(dim1, AIU_2BYTE_CELLS_PER_STICK) *
///                           AIU_PAGESIZE_IN_BYTES;
/// where CEIL(a, b) = (a + b - 1) / b,
///       AIU_STICKS_PER_PAGE = 32,
///       AIU_2BYTE_CELLS_PER_STICK = 64,
///       AIU_PAGESIZE_IN_BYTES = 4K.
///
/// timestep and batchsize are obtained from the LSTM/GRU input tensor.
/// hidden_size is obtained from the LSTM/GRU initial hidden tensor.
static Value insertAllocAndDeallocWorkAreaForRNNOps(PatternRewriter &rewriter,
    Location loc, Value rnnInput, Value rnnHiddenWeight, unsigned numOfGates,
    bool isDouble) {
  Value alloc;

  MemRefBoundsIndexCapture inputBounds(rnnInput);
  MemRefBoundsIndexCapture hiddenWeightBounds(rnnHiddenWeight);
  IndexExprScope scope(&rewriter, loc);
  DimIndexExpr timestepExp(inputBounds.getDim(0));
  IndexExpr Lit2 = LiteralIndexExpr(2);
  IndexExpr NumOfGatesLit = LiteralIndexExpr(numOfGates);
  DimIndexExpr dim1(hiddenWeightBounds.getDim(1));
  DimIndexExpr dim2(inputBounds.getDim(1));
  IndexExpr dim3 = LiteralIndexExpr(1);
  IndexExpr dim4 = NumOfGatesLit * timestepExp + NumOfGatesLit + Lit2;

  IndexExpr Lit1 = LiteralIndexExpr(1);
  IndexExpr Lit32 = LiteralIndexExpr(32);
  IndexExpr Lit64 = LiteralIndexExpr(64);
  IndexExpr Lit4K = LiteralIndexExpr(4096);
  IndexExpr ceilDim2 = (dim2 + Lit32 - Lit1).floorDiv(Lit32);
  IndexExpr ceilDim1 = (dim1 + Lit64 - Lit1).floorDiv(Lit64);
  IndexExpr sizeExpr = dim4 * dim3 * ceilDim2 * ceilDim1 * Lit4K;

  // Double the work area if required.
  if (isDouble)
    sizeExpr = sizeExpr * Lit2;

  // Emit alloc and dealloc ops.
  int64_t size = sizeExpr.isLiteral() ? sizeExpr.getLiteral() : -1;
  MemRefType resultType = MemRefType::get({size}, rewriter.getIntegerType(8));
  SmallVector<IndexExpr> dims(1, sizeExpr);
  alloc = insertAllocAndDeallocSimple(rewriter, nullptr, resultType, loc, dims,
      /*insertDealloc*/ ONNXToKrnl_gEmitDealloc, gAlignment);
  return alloc;
}

/// This function emits a buffer of zero elements for the given dimensions and
/// layout. If the given dimensions are static, then a stickified constant is
/// returned.
Value insertAllocOrEmitZeroConstant(ArrayRef<IndexExpr> dims,
    ZTensorEncodingAttr::DataLayout layout, Operation *op,
    PatternRewriter &rewriter, Location loc) {
  Value res;
  bool allStaticDims =
      llvm::all_of(dims, [](IndexExpr ie) { return ie.isLiteral(); });
  if (allStaticDims) {
    // Construct a MemRefType for the given dimensions and element type.
    SmallVector<int64_t, 4> shape;
    for (IndexExpr d : dims)
      shape.emplace_back(d.getLiteral());
    RankedTensorType tensorType =
        RankedTensorType::get(shape, rewriter.getF32Type(),
            ZTensorEncodingAttr::get(op->getContext(), layout));
    ZMemRefType zMemRefType = convertZTensorToMemRefType(rewriter, tensorType);
    MemRefType resType =
        normalizeMemRefType(zMemRefType.value.cast<MemRefType>(), rewriter,
            /*numSymbolicOperands=*/0);

    // Create a ZHighStickifiedConstantOp.
    ZHighStickifiedConstantOp stickifiedConstant =
        rewriter.create<ZHighStickifiedConstantOp>(loc, resType,
            /*value=*/nullptr,
            /*alignment=*/rewriter.getI64IntegerAttr(4096));

    // Use an opaque attribute to store stickified data.
    // Attribute type: tensor<sizeInBytes x i8>
    int64_t sizeInBytes = getMemRefSizeInBytes(resType).getValue();
    char *rawData = (char *)malloc(sizeInBytes);
    memset(rawData, 0, sizeInBytes);
    OpaqueElementsAttr valueAttr =
        OpaqueElementsAttr::get(stickifiedConstant.getOperation()->getDialect(),
            RankedTensorType::get({sizeInBytes}, rewriter.getI8Type()),
            StringRef(rawData, sizeInBytes));
    stickifiedConstant.valueAttr(valueAttr);
    free(rawData);

    res = stickifiedConstant.getResult();
  } else {
    MultiDialectBuilder<KrnlBuilder, MathBuilder> create(rewriter, loc);
    res = insertAllocAndDeallocZMemRefByDim(dims, layout, op, rewriter);
    Value initValue = create.math.constant(rewriter.getF16Type(), 0);
    create.krnl.memset(res, initValue);
  }
  return res;
}

/// Emit instructions to allocate a buffer to store original dimensions.
Value insertShapeMemRefI64(
    PatternRewriter &rewriter, Location loc, ArrayRef<IndexExpr> originalDims) {
  MultiDialectBuilder<KrnlBuilder, MathBuilder> create(rewriter, loc);
  MemRefType shapeMemRefType = MemRefType::get(
      {(int64_t)originalDims.size()}, rewriter.getIntegerType(64));
  Value shapeMemRef = insertAllocAndDealloc(
      shapeMemRefType, loc, rewriter, ONNXToKrnl_gEmitDealloc);
  for (uint64_t i = 0; i < originalDims.size(); ++i) {
    Value dim =
        create.math.cast(rewriter.getI64Type(), originalDims[i].getValue());
    create.krnl.storeIE(dim, shapeMemRef, {LiteralIndexExpr(i)});
  }
  return shapeMemRef;
}

/// Get the corresponding MemRefType and layout of a given ZTensorType.
ZMemRefType convertZTensorToMemRefType(OpBuilder b, Type type) {
  ZMemRefType resZMemRefType;
  if (type.isa<TensorType>()) {
    RankedTensorType tensorType = type.dyn_cast<RankedTensorType>();
    assert(tensorType && "expected only ranked shapes");
    ArrayRef<int64_t> shape = tensorType.getShape();
    Type elementType = tensorType.getElementType();
    int64_t rank = shape.size();
    if (tensorType.getEncoding()) {
      // Obtain element type and affine map.
      AffineExpr constExpr0 = getAffineConstantExpr(0, b.getContext());
      AffineExpr constExpr31 = getAffineConstantExpr(31, b.getContext());
      AffineExpr constExpr32 = getAffineConstantExpr(32, b.getContext());
      AffineExpr constExpr64 = getAffineConstantExpr(64, b.getContext());
      unsigned e4, e3, e2, e1;
      AffineExpr n, c, h, w, res32, res64;
      SmallVector<AffineExpr, 6> dimExpr;

      ZTensorEncodingAttr::DataLayout layout = getZTensorLayout(tensorType);
      if (layout == ZTensorEncodingAttr::DataLayout::_1D) {
        // (e1) -> (1, 1, 1, e1) -> (1, ceil(e1/64), 1, 1, 32, 64)
        e1 = 0;
        n = constExpr0;
        h = b.getAffineDimExpr(e1).floorDiv(constExpr64);
        w = constExpr0;
        c = constExpr0;
        res32 = constExpr31;
        res64 = b.getAffineDimExpr(e1) % constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::_2D) {
        // (e2, e1) -> (1, 1, e2, e1) -> (1, ceil(e1/64), 1, ceil(e2/32), 32
        // 64)
        e2 = 0;
        e1 = 1;
        n = constExpr0;
        h = b.getAffineDimExpr(e1).floorDiv(constExpr64);
        w = constExpr0;
        c = b.getAffineDimExpr(e2).floorDiv(constExpr32);
        res32 = b.getAffineDimExpr(e2) % constExpr32;
        res64 = b.getAffineDimExpr(e1) % constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::_3D) {
        // (e3, e2, e1) -> (1, e3, e2, e1)
        // -> (1, ceil(e1/64), e3, ceil(e2/32), 32, 64)
        e3 = 0;
        e2 = 1;
        e1 = 2;
        n = constExpr0;
        h = b.getAffineDimExpr(e1).floorDiv(constExpr64);
        w = b.getAffineDimExpr(e3);
        c = b.getAffineDimExpr(e2).floorDiv(constExpr32);
        res32 = b.getAffineDimExpr(e2) % constExpr32;
        res64 = b.getAffineDimExpr(e1) % constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::_4D) {
        // (e4, e3, e2, e1) -> (e4, ceil(e1/64), e3, ceil(e2/32), 32, 64)
        e4 = 0;
        e3 = 1;
        e2 = 2;
        e1 = 3;
        n = b.getAffineDimExpr(e4);
        h = b.getAffineDimExpr(e1).floorDiv(constExpr64);
        w = b.getAffineDimExpr(e3);
        c = b.getAffineDimExpr(e2).floorDiv(constExpr32);
        res32 = b.getAffineDimExpr(e2) % constExpr32;
        res64 = b.getAffineDimExpr(e1) % constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::_2DS) {
        // (e4, e1) -> (e4, 1, 1, e1) -> (e4, ceil(e1/64), 1, 1, 32, 64)
        e4 = 0;
        e1 = 1;
        n = b.getAffineDimExpr(e4);
        h = b.getAffineDimExpr(e1).floorDiv(constExpr64);
        w = constExpr0;
        c = constExpr0;
        res32 = constExpr31;
        res64 = b.getAffineDimExpr(e1) % constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::_3DS) {
        // (e4, e2, e1) -> (e4, 1, e2, e1)
        // -> (e4, ceil(e1/64), 1, ceil(e2/32), 32, 64)
        e4 = 0;
        e2 = 1;
        e1 = 2;
        n = b.getAffineDimExpr(e4);
        h = b.getAffineDimExpr(e1).floorDiv(constExpr64);
        w = constExpr0;
        c = b.getAffineDimExpr(e2).floorDiv(constExpr32);
        res32 = b.getAffineDimExpr(e2) % constExpr32;
        res64 = b.getAffineDimExpr(e1) % constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::_4DS) {
      } else if (layout == ZTensorEncodingAttr::DataLayout::NHWC) {
        // (e4, e3, e2, e1) -> (e4, ceil(e1/64), e3, ceil(e2/32), 32, 64)
        e4 = 0;
        e3 = 1;
        e2 = 2;
        e1 = 3;
        n = b.getAffineDimExpr(e4);
        h = b.getAffineDimExpr(e1).floorDiv(constExpr64);
        w = b.getAffineDimExpr(e3);
        c = b.getAffineDimExpr(e2).floorDiv(constExpr32);
        res32 = b.getAffineDimExpr(e2) % constExpr32;
        res64 = b.getAffineDimExpr(e1) % constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::NCHW) {
        // (e4, e3, e2, e1) -> (e4, ceil(e2/64), e1, ceil(e3/32), 32, 64)
        llvm_unreachable("Not tested yet");
        e4 = 0;
        e3 = 1;
        e2 = 2;
        e1 = 3;
        n = b.getAffineDimExpr(e4);
        h = b.getAffineDimExpr(e2).floorDiv(constExpr64);
        w = b.getAffineDimExpr(e1);
        c = b.getAffineDimExpr(e3).floorDiv(constExpr32);
        res32 = b.getAffineDimExpr(e3) % constExpr32;
        res64 = b.getAffineDimExpr(e2) % constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::HWCK) {
        // HWCK (e4, e3, e2, e1) -> KHWC (ceil(e1/64), e4,, e3, ceil(e2/32),
        // 32, 64)
        e4 = 0;
        e3 = 1;
        e2 = 2;
        e1 = 3;
        n = b.getAffineDimExpr(e1).floorDiv(constExpr64);
        h = b.getAffineDimExpr(e4);
        w = b.getAffineDimExpr(e3);
        c = b.getAffineDimExpr(e2).floorDiv(constExpr32);
        res32 = b.getAffineDimExpr(e2) % constExpr32;
        res64 = b.getAffineDimExpr(e1) % constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::FICO) {
        // (e4, e3, e2, e1) -> (e4, 4*ceil(e1/4/64), e3, ceil(e2/32), 32, 64)
        assert(shape[rank - 1] != -1 && (shape[rank - 1] % 4) == 0 &&
               "wrong concatenated dimension size");
        int64_t s = shape[rank - 1] / 4;
        // ((s + 64 - 1) / 64) * 64;
        int64_t s_pad = ceil((double)s / 64) * 64;
        int64_t pad_size = s_pad - s;
        AffineExpr constExprS = getAffineConstantExpr(s, b.getContext());
        if (rank == 2) {
          e2 = 0;
          e1 = 1;
          w = constExpr0;
        } else if (rank == 3) {
          e3 = 0;
          e2 = 1;
          e1 = 2;
          w = b.getAffineDimExpr(e3);
        } else {
          llvm_unreachable("Unsupported rank in ZDNN_FICO layout");
        }
        n = constExpr0;
        h = (b.getAffineDimExpr(e1) +
             pad_size * (b.getAffineDimExpr(e1).floorDiv(constExprS)))
                .floorDiv(constExpr64);
        c = b.getAffineDimExpr(e2).floorDiv(constExpr32);
        res32 = b.getAffineDimExpr(e2) % constExpr32;
        res64 = (b.getAffineDimExpr(e1) +
                    pad_size * (b.getAffineDimExpr(e1).floorDiv(constExprS))) %
                constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::ZRH) {
        // (e4, e3, e2, e1) -> (e4, 3*ceil(e1/4/64), e3, ceil(e2/32), 32, 64)
        int64_t hidden_size = shape[rank - 1];
        assert(hidden_size > 0 &&
               "Dynamic dimension in hidden_size not supported "
               "in affine_map generation.");
        assert((hidden_size % 3) == 0 && "wrong concatenated dimension size.");
        int64_t s = hidden_size / 3;
        int64_t s_pad = ceil((float)s / 64) * 64; // ((s + 64 - 1) / 64) * 64;
        int64_t pad_size = s_pad - s;
        AffineExpr constExprS = getAffineConstantExpr(s, b.getContext());
        if (rank == 2) {
          e2 = 0;
          e1 = 1;
          w = constExpr0;
        } else if (rank == 3) {
          e3 = 0;
          e2 = 1;
          e1 = 2;
          w = b.getAffineDimExpr(e3);
        } else {
          llvm_unreachable("Unsupported rank in ZDNN_ZRH layout");
        }
        n = constExpr0;
        h = (b.getAffineDimExpr(e1) +
             pad_size * (b.getAffineDimExpr(e1).floorDiv(constExprS)))
                .floorDiv(constExpr64);
        c = b.getAffineDimExpr(e2).floorDiv(constExpr32);
        res32 = b.getAffineDimExpr(e2) % constExpr32;
        res64 = (b.getAffineDimExpr(e1) +
                    pad_size * (b.getAffineDimExpr(e1).floorDiv(constExprS))) %
                constExpr64;
      } else if (layout == ZTensorEncodingAttr::DataLayout::BFICO) {
        llvm_unreachable("Unsupported layout yet");
      } else if (layout == ZTensorEncodingAttr::DataLayout::BZRH) {
        llvm_unreachable("Unsupported layout yet");
      } else
        llvm_unreachable("Unsupported layout");

      dimExpr.emplace_back(n);
      dimExpr.emplace_back(h);
      dimExpr.emplace_back(w);
      dimExpr.emplace_back(c);
      dimExpr.emplace_back(res32);
      dimExpr.emplace_back(res64);
      AffineMap smap = AffineMap::get(rank, 0, dimExpr, b.getContext());
      // Output type is F16 for zAIU.
      MemRefType outType = MemRefType::get(shape, b.getF16Type());
      resZMemRefType.value =
          MemRefType::Builder(outType).setLayout(AffineMapAttr::get(smap));
      resZMemRefType.layout = convertDataLayoutToStringAttr(b, layout);
    } else {
      resZMemRefType.value = MemRefType::get(shape, elementType);
    }
  } else {
    resZMemRefType.value = type.dyn_cast<MemRefType>();
  }
  return resZMemRefType;
}

//===----------------------------------------------------------------------===//
// Lower ZHigh Stick to ZLow Stick
//===----------------------------------------------------------------------===//

struct ZHighToZLowStickOpLowering : public ConversionPattern {
  ZHighToZLowStickOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(
            typeConverter, mlir::ZHighStickOp::getOperationName(), 1, ctx) {}

  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighStickOpAdaptor operandAdaptor(operands);
    Value input = operandAdaptor.In();

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Allocate a buffer for the result MemRef.
    MemRefBoundsIndexCapture inputBounds(input);
    IndexExprScope scope(&rewriter, loc);
    SmallVector<IndexExpr, 4> dims;
    inputBounds.getDimList(dims);
    Value alloc = insertAllocAndDeallocZMemRef(zMemRefType, dims, op, rewriter);

    // Emit a ZLow operation.
    rewriter.create<ZLowStickOp>(loc, input, alloc, zMemRefType.layout);

    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh StickForLSTM to ZLow StickForLSTM
//===----------------------------------------------------------------------===//

struct ZHighToZLowStickForLSTMOpLowering : public ConversionPattern {
  ZHighToZLowStickForLSTMOpLowering(
      TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(typeConverter,
            mlir::ZHighStickForLSTMOp::getOperationName(), 1, ctx) {}

  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighStickForLSTMOp stickOp = llvm::dyn_cast<ZHighStickForLSTMOp>(op);
    ZHighStickForLSTMOpAdaptor operandAdaptor(operands);

    ZHighStickForLSTMOpShapeHelper shapeHelper(&stickOp, &rewriter);
    LogicalResult shapecomputed = shapeHelper.computeShape(operandAdaptor);
    assert(succeeded(shapecomputed));

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Allocate a buffer for the result MemRef.
    Value alloc = insertAllocAndDeallocZMemRef(
        zMemRefType, shapeHelper.dimsForOutput(0), op, rewriter);

    // Emit a ZLow operation.
    rewriter.create<ZLowStickForLSTMOp>(loc, operandAdaptor.f_gate(),
        operandAdaptor.i_gate(), operandAdaptor.c_gate(),
        operandAdaptor.o_gate(), alloc);

    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh StickForGRU to ZLow StickForGRU
//===----------------------------------------------------------------------===//

struct ZHighToZLowStickForGRUOpLowering : public ConversionPattern {
  ZHighToZLowStickForGRUOpLowering(
      TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(typeConverter,
            mlir::ZHighStickForGRUOp::getOperationName(), 1, ctx) {}

  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighStickForGRUOp stickOp = llvm::dyn_cast<ZHighStickForGRUOp>(op);
    ZHighStickForGRUOpAdaptor operandAdaptor(operands);

    ZHighStickForGRUOpShapeHelper shapeHelper(&stickOp, &rewriter);
    LogicalResult shapecomputed = shapeHelper.computeShape(operandAdaptor);
    assert(succeeded(shapecomputed));

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Allocate a buffer for the result MemRef.
    Value alloc = insertAllocAndDeallocZMemRef(
        zMemRefType, shapeHelper.dimsForOutput(0), op, rewriter);

    // Emit a ZLow operation.
    rewriter.create<ZLowStickForGRUOp>(loc, operandAdaptor.z_gate(),
        operandAdaptor.r_gate(), operandAdaptor.h_gate(), alloc);

    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh Unstick to ZLow Unstick
//===----------------------------------------------------------------------===//

struct ZHighToZLowUnstickOpLowering : public ConversionPattern {
  ZHighToZLowUnstickOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(
            typeConverter, mlir::ZHighUnstickOp::getOperationName(), 1, ctx) {}

  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighUnstickOpAdaptor operandAdaptor(operands);
    Value input = operandAdaptor.In();

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Allocate a buffer for the result MemRef.
    MemRefBoundsIndexCapture inputBounds(input);
    IndexExprScope scope(&rewriter, loc);
    SmallVector<IndexExpr, 4> dims;
    inputBounds.getDimList(dims);
    Value alloc = insertAllocAndDeallocZMemRef(
        zMemRefType, dims, op, rewriter, /*alignment=*/-1);

    // Emit a ZLow operation.
    rewriter.create<ZLowUnstickOp>(loc, input, alloc, readLayout(input));
    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh Stickified Constant to KrnlGlobal
//===----------------------------------------------------------------------===//

struct ZHighToZLowStickifiedConstantOpLowering : public ConversionPattern {
  static int constantID;
  ZHighToZLowStickifiedConstantOpLowering(
      TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(typeConverter,
            mlir::ZHighStickifiedConstantOp::getOperationName(), 1, ctx) {}

  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighStickifiedConstantOp stickifiedConstOp =
        llvm::dyn_cast<ZHighStickifiedConstantOp>(op);

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Normalize MemRefType to get a static shape.
    assert(zMemRefType.value.cast<MemRefType>().getNumDynamicDims() == 0 &&
           "MemRefType has dynamic dimensions");
    MemRefType normalizedType =
        normalizeMemRefType(zMemRefType.value.cast<MemRefType>(), rewriter,
            /*numSymbolicOperands=*/0);
    ArrayRef<int64_t> normalizedShape = normalizedType.getShape();

    // Get Opaque attribute.
    StringRef data = stickifiedConstOp.value()
                         .getValue()
                         .cast<OpaqueElementsAttr>()
                         .getValue();

    // Validate the stickified tensor.
    int64_t memRefSizeInBytes = getMemRefEltSizeInBytes(normalizedType);
    memRefSizeInBytes *= normalizedType.getNumElements();
    assert((data.size() == (uint64_t)memRefSizeInBytes) &&
           "The stickied tensor's buffer size and MemRef's size mismatched");

    // Create a KrnlGlobalOp.
    KrnlGlobalOp constantGlobal =
        rewriter.create<KrnlGlobalOp>(loc, zMemRefType.value,
            /*shape=*/
            rewriter.getI64ArrayAttr(normalizedShape),
            /*name=*/
            rewriter.getStringAttr(
                "constant_stickify_" + std::to_string(constantID)),
            /*value=*/stickifiedConstOp.valueAttr(),
            /*offset=*/nullptr,
            /*alignment=*/stickifiedConstOp.alignmentAttr());

    // Increment constant ID:
    constantID++;

    rewriter.replaceOp(op, constantGlobal.getResult());
    return success();
  }
};

int ZHighToZLowStickifiedConstantOpLowering::constantID = 0;

template <typename OP_TYPE>
struct ZLowOpFor {
  using Op = void;
};

//===----------------------------------------------------------------------===//
// Lower ZHigh binary ops to ZLow.
//===----------------------------------------------------------------------===//

template <>
struct ZLowOpFor<ZHighAddOp> {
  using Op = ZLowAddOp;
};

template <>
struct ZLowOpFor<ZHighSubOp> {
  using Op = ZLowSubOp;
};

template <>
struct ZLowOpFor<ZHighMulOp> {
  using Op = ZLowMulOp;
};

template <>
struct ZLowOpFor<ZHighDivOp> {
  using Op = ZLowDivOp;
};

template <>
struct ZLowOpFor<ZHighMinOp> {
  using Op = ZLowMinOp;
};

template <>
struct ZLowOpFor<ZHighMaxOp> {
  using Op = ZLowMaxOp;
};

template <typename OP_TYPE>
struct ZHighToZLowBinaryOpLowering : public ConversionPattern {
  ZHighToZLowBinaryOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(typeConverter, OP_TYPE::getOperationName(), 1, ctx) {}

  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    Value inputA = operands[0];
    Value inputB = operands[1];

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Allocate a buffer for the result MemRef.
    MemRefBoundsIndexCapture inputBounds(inputA);
    IndexExprScope scope(&rewriter, loc);
    SmallVector<IndexExpr, 4> dims;
    inputBounds.getDimList(dims);
    Value alloc = insertAllocAndDeallocZMemRef(zMemRefType, dims, op, rewriter);

    // Get the original shape before it is vanished by lower passes.
    Value shape = insertShapeMemRefI64(rewriter, loc, dims);
    rewriter.create<typename ZLowOpFor<OP_TYPE>::Op>(
        loc, inputA, inputB, shape, alloc, zMemRefType.layout);
    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh unary ops to ZLow.
//===----------------------------------------------------------------------===//

template <>
struct ZLowOpFor<ZHighLogOp> {
  using Op = ZLowLogOp;
};

template <>
struct ZLowOpFor<ZHighExpOp> {
  using Op = ZLowExpOp;
};

template <>
struct ZLowOpFor<ZHighReluOp> {
  using Op = ZLowReluOp;
};

template <>
struct ZLowOpFor<ZHighTanhOp> {
  using Op = ZLowTanhOp;
};

template <>
struct ZLowOpFor<ZHighSigmoidOp> {
  using Op = ZLowSigmoidOp;
};

template <typename OP_TYPE>
struct ZHighToZLowUnaryOpLowering : public ConversionPattern {
  ZHighToZLowUnaryOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(OP_TYPE::getOperationName(), 1, ctx) {}

  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    Value input = operands[0];

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Allocate a buffer for the result MemRef.
    MemRefBoundsIndexCapture inputBounds(input);
    IndexExprScope scope(&rewriter, loc);
    SmallVector<IndexExpr, 4> dims;
    inputBounds.getDimList(dims);
    Value alloc = insertAllocAndDeallocZMemRef(zMemRefType, dims, op, rewriter);

    // Get the original shape before it is vanished by lower passes.
    Value shape = insertShapeMemRefI64(rewriter, loc, dims);

    // Emit a ZLow operation.
    rewriter.create<typename ZLowOpFor<OP_TYPE>::Op>(
        loc, input, shape, alloc, zMemRefType.layout);
    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh Softmax to ZLow Softmax
//===----------------------------------------------------------------------===//
struct ZHighToZLowSoftmaxOpLowering : public ConversionPattern {
  ZHighToZLowSoftmaxOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(
            typeConverter, mlir::ZHighSoftmaxOp::getOperationName(), 1, ctx) {}
  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighSoftmaxOp softmaxOp = llvm::dyn_cast<ZHighSoftmaxOp>(op);
    ZHighSoftmaxOpAdaptor operandAdaptor(operands);
    Value input = operands[0];

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Allocate a buffer for the result MemRef.
    MemRefBoundsIndexCapture inputBounds(input);
    IndexExprScope scope(&rewriter, loc);
    SmallVector<IndexExpr, 4> dims;
    inputBounds.getDimList(dims);
    Value alloc = insertAllocAndDeallocZMemRef(zMemRefType, dims, op, rewriter);

    // Get the original shape before it is vanished by lower passes.
    Value shape = insertShapeMemRefI64(rewriter, loc, dims);

    // Emit 'alloc' and 'dealloc' for work_area that is of 4K-aligned 8K bytes.
    Value workArea = insertAllocAndDealloc(
        MemRefType::get({8 * 1024}, rewriter.getIntegerType(8)), loc, rewriter,
        /*insertDealloc=*/ONNXToKrnl_gEmitDealloc, {},
        /*alignment=*/gAlignment);

    // Emit ZLow.softmax.
    rewriter.create<ZLowSoftmaxOp>(
        loc, input, workArea, shape, alloc, softmaxOp.act_funcAttr());
    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh MeanReduce2D to ZLow MeanReduce2D
//===----------------------------------------------------------------------===//
struct ZHighToZLowMeanReduce2DOpLowering : public ConversionPattern {
  ZHighToZLowMeanReduce2DOpLowering(
      TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(typeConverter,
            mlir::ZHighMeanReduce2DOp::getOperationName(), 1, ctx) {}
  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighMeanReduce2DOpAdaptor operandAdaptor(operands);
    Value input = operandAdaptor.input();

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Allocate a buffer for the result MemRef.
    MemRefBoundsIndexCapture inputBounds(input);
    IndexExprScope scope(&rewriter, loc);
    SmallVector<IndexExpr, 4> dims;
    inputBounds.getDimList(dims);

    // Get the original shape before it is vanished by lower passes.
    Value shape = insertShapeMemRefI64(rewriter, loc, dims);

    // Input is NHWC, and H and W are reduction dimensions.
    dims[1] = LiteralIndexExpr(1);
    dims[2] = LiteralIndexExpr(1);
    Value alloc = insertAllocAndDeallocZMemRef(zMemRefType, dims, op, rewriter);

    rewriter.create<ZLowMeanReduce2DOp>(loc, input, shape, alloc);
    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh Pool2D to ZLow Pool2D
//===----------------------------------------------------------------------===//
template <typename ZHIGHPOOLOP, typename ZHIGHADAPTOR, typename ZLOWPOOLOP>
struct ZHighToZLowPool2DOpLowering : public ConversionPattern {
  ZHighToZLowPool2DOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(
            typeConverter, ZHIGHPOOLOP::getOperationName(), 1, ctx) {}
  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHIGHPOOLOP pool2dOp = llvm::dyn_cast<ZHIGHPOOLOP>(op);
    ZHIGHADAPTOR operandAdaptor(operands);

    // Helper builders.
    MultiDialectBuilder<KrnlBuilder, MathBuilder> create(rewriter, loc);
    IndexExprScope scope(create.krnl);

    // Infer shape.
    ZHighPoolingOpShapeHelper<ZHIGHPOOLOP, ZHIGHADAPTOR> shapeHelper(
        &pool2dOp, &rewriter);
    LogicalResult shapecomputed = shapeHelper.computeShape(operandAdaptor);
    assert(succeeded(shapecomputed));

    // Convert type.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, pool2dOp.getResult().getType());

    // Allocate result buffers.
    Value alloc = insertAllocAndDeallocZMemRef(
        zMemRefType, shapeHelper.dimsForOutput(0), op, rewriter);

    // Create a buffer to store the original shape information.
    Value shapeMemRef =
        insertShapeMemRefI64(rewriter, loc, shapeHelper.allOriginalDims);

    // Create a zLow op.
    rewriter.create<ZLOWPOOLOP>(loc, operandAdaptor.input(), shapeMemRef, alloc,
        pool2dOp.kernel_shapeAttr(), pool2dOp.stridesAttr(),
        pool2dOp.padding_typeAttr());
    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh MatMul to ZLow MatMul
//===----------------------------------------------------------------------===//

struct ZHighToZLowMatMulOpLowering : public ConversionPattern {
  ZHighToZLowMatMulOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(
            typeConverter, mlir::ZHighMatMulOp::getOperationName(), 1, ctx) {}

  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighMatMulOp matmulOp = llvm::dyn_cast<ZHighMatMulOp>(op);
    ZHighMatMulOpAdaptor operandAdaptor(operands);

    MultiDialectBuilder<KrnlBuilder, MathBuilder> create(rewriter, loc);
    IndexExprScope scope(create.krnl);

    ZHighMatMulOpShapeHelper shapeHelper(&matmulOp, &rewriter);
    LogicalResult shapecomputed = shapeHelper.computeShape(operandAdaptor);
    assert(succeeded(shapecomputed));

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    Value alloc = insertAllocAndDeallocZMemRef(
        zMemRefType, shapeHelper.dimsForOutput(0), op, rewriter);

    // Get the original shape before it is vanished by lower passes.
    // Create a 1D MemRef containing necessary dimensions for construcing
    // original shapes.
    // - In case of unstacked: X(m, n) * Y(n, p) + Bias(p)
    // shape is a 1D MemRef (memref<3xindex>) whose items are:
    //   - 1st item: m
    //   - 2nd item: n
    //   - 3rd item: p
    // - In case of stacked: X(s, m, n) * Y(s, n, p) + Bias(s, p)
    //      or broadcasting: X(s, m, n) * Y(n, p) + Bias(p)
    // shape is a 1D MemRef (memref<4xindex>) whose items are:
    //   - 1st item: s
    //   - 2nd item: m
    //   - 3rd item: n
    //   - 4th item: p

    Value shapeMemRef =
        insertShapeMemRefI64(rewriter, loc, shapeHelper.allOriginalDims);

    // Prepare optional bias.
    Value bias = operandAdaptor.B();
    if (isNoneType(bias)) {
      MemRefBoundsIndexCapture resBounds(alloc);
      SmallVector<IndexExpr, 2> biasDims;
      ZTensorEncodingAttr::DataLayout biasLayout;
      if (shapeHelper.isStacked) {
        // Bias type is 2DS.
        biasDims.emplace_back(resBounds.getDim(0));
        biasDims.emplace_back(resBounds.getDim(2));
        biasLayout = ZTensorEncodingAttr::DataLayout::_2DS;
      } else {
        // Bias type is 1D. Get the last dim size.
        biasDims.emplace_back(resBounds.getDim(resBounds.getRank() - 1));
        biasLayout = ZTensorEncodingAttr::DataLayout::_1D;
      }
      // Allocate bias.
      bias = insertAllocOrEmitZeroConstant(
          biasDims, biasLayout, op, rewriter, loc);
    }

    // Attributes.
    int64_t bcast = (shapeHelper.isBroadcasted) ? -1 : 0;
    int64_t stacked = (shapeHelper.isStacked) ? -1 : 0;
    IntegerAttr is_bcastAttr =
        rewriter.getIntegerAttr(rewriter.getIntegerType(64, true), bcast);
    IntegerAttr is_stackedAttr =
        rewriter.getIntegerAttr(rewriter.getIntegerType(64, true), stacked);

    // Emit zlow.matmul.
    rewriter.create<ZLowMatMulOp>(loc, operandAdaptor.X(), operandAdaptor.Y(),
        bias, shapeMemRef, alloc, is_bcastAttr, is_stackedAttr);
    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh LSTM to ZLow LSTM
//===----------------------------------------------------------------------===//
struct ZHighToZLowLSTMOpLowering : public ConversionPattern {
  ZHighToZLowLSTMOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(
            typeConverter, mlir::ZHighLSTMOp::getOperationName(), 1, ctx) {}
  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighLSTMOp lstmOp = llvm::dyn_cast<mlir::ZHighLSTMOp>(op);
    ZHighLSTMOpAdaptor operandAdaptor(operands);

    // Helper builders.
    MultiDialectBuilder<KrnlBuilder, MathBuilder> create(rewriter, loc);
    IndexExprScope scope(create.krnl);

    // Infer shape.
    ZHighLSTMOpShapeHelper shapeHelper(&lstmOp, &rewriter);
    LogicalResult shapecomputed = shapeHelper.computeShape(operandAdaptor);
    assert(succeeded(shapecomputed));

    // Convert type.
    ZMemRefType hnZMemRefType =
        convertZTensorToMemRefType(rewriter, lstmOp.getResults()[0].getType());
    ZMemRefType cfZMemRefType =
        convertZTensorToMemRefType(rewriter, lstmOp.getResults()[1].getType());

    // Allocate result buffers.
    Value allocHnOutput = insertAllocAndDeallocZMemRef(
        hnZMemRefType, shapeHelper.dimsForOutput(0), op, rewriter);
    Value allocCfOutput = insertAllocAndDeallocZMemRef(
        cfZMemRefType, shapeHelper.dimsForOutput(1), op, rewriter);

    // Get the original shape before it is vanished by lower passes.
    // Create a 1D MemRef containing necessary dimensions for construcing
    // original shapes.
    // shapeMemRef :: memref<5xindex>
    // - 1st item: direction
    // - 2nd item: timestep
    // - 3rd item: batchSize
    // - 4th item: featureSize
    // - 5th item: hiddenSize
    Value shapeMemRef =
        insertShapeMemRefI64(rewriter, loc, shapeHelper.allOriginalDims);

    // Prepare optional values: input_bias, hidden_bias, initial_h, initial_c.
    Value initial_h = operandAdaptor.h0();
    Value initial_c = operandAdaptor.c0();
    Value input_bias = operandAdaptor.input_bias();
    Value hidden_bias = operandAdaptor.hidden_bias();
    if (initial_h.getType().isa<NoneType>()) {
      initial_h = insertAllocOrEmitZeroConstant(shapeHelper.hc0Shape,
          ZTensorEncodingAttr::DataLayout::_3DS, op, rewriter, loc);
    }
    if (initial_c.getType().isa<NoneType>()) {
      initial_c = insertAllocOrEmitZeroConstant(shapeHelper.hc0Shape,
          ZTensorEncodingAttr::DataLayout::_3DS, op, rewriter, loc);
    }
    if (input_bias.getType().isa<NoneType>()) {
      input_bias = insertAllocOrEmitZeroConstant(shapeHelper.biasShape,
          ZTensorEncodingAttr::DataLayout::FICO, op, rewriter, loc);
    }
    if (hidden_bias.getType().isa<NoneType>()) {
      hidden_bias = insertAllocOrEmitZeroConstant(shapeHelper.biasShape,
          ZTensorEncodingAttr::DataLayout::FICO, op, rewriter, loc);
    }

    // Prepare work area. Double the area for the bidirectional mode.
    bool isDouble =
        lstmOp.directionAttr().getValue().equals_insensitive("bidirectional");
    Value workArea = insertAllocAndDeallocWorkAreaForRNNOps(rewriter, loc,
        operandAdaptor.input(), operandAdaptor.hidden_weights(),
        /*numOfGates=*/4,
        /*isDouble=*/isDouble);

    // Emit zlow.lstm.
    rewriter.create<ZLowLSTMOp>(loc, operandAdaptor.input(), initial_h,
        initial_c, operandAdaptor.input_weights(), input_bias,
        operandAdaptor.hidden_weights(), hidden_bias, workArea, shapeMemRef,
        allocHnOutput, allocCfOutput, lstmOp.directionAttr(),
        lstmOp.return_all_stepsAttr());
    std::vector<Value> outputs = {allocHnOutput, allocCfOutput};
    rewriter.replaceOp(op, outputs);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh GRU to ZLow GRU
//===----------------------------------------------------------------------===//
struct ZHighToZLowGRUOpLowering : public ConversionPattern {
  ZHighToZLowGRUOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(
            typeConverter, mlir::ZHighGRUOp::getOperationName(), 1, ctx) {}
  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighGRUOp gruOp = llvm::dyn_cast<mlir::ZHighGRUOp>(op);
    ZHighGRUOpAdaptor operandAdaptor(operands);

    // Helper builders.
    MultiDialectBuilder<KrnlBuilder, MathBuilder> create(rewriter, loc);
    IndexExprScope scope(create.krnl);

    // Infer shape.
    ZHighGRUOpShapeHelper shapeHelper(&gruOp, &rewriter);
    LogicalResult shapecomputed = shapeHelper.computeShape(operandAdaptor);
    assert(succeeded(shapecomputed));

    // Convert type.
    ZMemRefType hnZMemRefType =
        convertZTensorToMemRefType(rewriter, gruOp.getResult().getType());

    // Allocate result buffers.
    Value allocHnOutput = insertAllocAndDeallocZMemRef(
        hnZMemRefType, shapeHelper.dimsForOutput(0), op, rewriter);

    // Get the original shape before it is vanished by lower passes.
    // Create a 1D MemRef containing necessary dimensions for construcing
    // original shapes.
    // shapeMemRef :: memref<5xindex>
    // - 1st item: direction
    // - 2nd item: timestep
    // - 3rd item: batchSize
    // - 4th item: featureSize
    // - 5th item: hiddenSize
    Value shapeMemRef =
        insertShapeMemRefI64(rewriter, loc, shapeHelper.allOriginalDims);

    // Prepare optional values: input_bias, hidden_bias, initial_h.
    Value initial_h = operandAdaptor.h0();
    Value input_bias = operandAdaptor.input_bias();
    Value hidden_bias = operandAdaptor.hidden_bias();
    if (initial_h.getType().isa<NoneType>()) {
      initial_h = insertAllocOrEmitZeroConstant(shapeHelper.h0Shape,
          ZTensorEncodingAttr::DataLayout::_3DS, op, rewriter, loc);
    }
    if (input_bias.getType().isa<NoneType>()) {
      input_bias = insertAllocOrEmitZeroConstant(shapeHelper.biasShape,
          ZTensorEncodingAttr::DataLayout::ZRH, op, rewriter, loc);
    }
    if (hidden_bias.getType().isa<NoneType>()) {
      hidden_bias = insertAllocOrEmitZeroConstant(shapeHelper.biasShape,
          ZTensorEncodingAttr::DataLayout::ZRH, op, rewriter, loc);
    }

    // Prepare work area. Double the area for the bidirectional mode.
    bool isDouble =
        gruOp.directionAttr().getValue().equals_insensitive("bidirectional");
    Value workArea = insertAllocAndDeallocWorkAreaForRNNOps(rewriter, loc,
        operandAdaptor.input(), operandAdaptor.hidden_weights(),
        /*numOfGates=*/3,
        /*isDouble=*/isDouble);

    // Emit zlow.gru.
    rewriter.create<ZLowGRUOp>(loc, operandAdaptor.input(), initial_h,
        operandAdaptor.input_weights(), input_bias,
        operandAdaptor.hidden_weights(), hidden_bias, workArea, shapeMemRef,
        allocHnOutput, gruOp.directionAttr(), gruOp.return_all_stepsAttr());
    rewriter.replaceOp(op, allocHnOutput);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh Conv2D to ZLow Conv2D
//===----------------------------------------------------------------------===//
struct ZHighToZLowConv2DOpLowering : public ConversionPattern {
  ZHighToZLowConv2DOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(
            typeConverter, mlir::ZHighConv2DOp::getOperationName(), 1, ctx) {}
  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighConv2DOp conv2dOp = llvm::dyn_cast<mlir::ZHighConv2DOp>(op);
    ZHighConv2DOpAdaptor operandAdaptor(operands);

    // Helper builders.
    MultiDialectBuilder<KrnlBuilder, MathBuilder> create(rewriter, loc);
    IndexExprScope scope(create.krnl);

    // Infer shape.
    ZHighConv2DOpShapeHelper shapeHelper(&conv2dOp, &rewriter);
    LogicalResult shapecomputed = shapeHelper.computeShape(operandAdaptor);
    assert(succeeded(shapecomputed));

    // Convert type.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, conv2dOp.getResult().getType());

    // Allocate result buffers.
    Value alloc = insertAllocAndDeallocZMemRef(
        zMemRefType, shapeHelper.dimsForOutput(0), op, rewriter);

    // Create a buffer to store the original shape information.
    Value shapeMemRef =
        insertShapeMemRefI64(rewriter, loc, shapeHelper.allOriginalDims);

    // Prepare optional values: input_bias.
    Value bias = operandAdaptor.input_bias();
    if (bias.getType().isa<NoneType>()) {
      // Bias's shape is [Channel_out].
      SmallVector<IndexExpr> dims(1, shapeHelper.allOriginalDims[4]);
      bias = insertAllocOrEmitZeroConstant(
          dims, ZTensorEncodingAttr::DataLayout::_1D, op, rewriter, loc);
    }

    // Create a zLow op.
    rewriter.create<ZLowConv2DOp>(loc, operandAdaptor.input(),
        operandAdaptor.input_kernel(), bias, shapeMemRef, alloc,
        conv2dOp.kernel_shapeAttr(), conv2dOp.stridesAttr(),
        conv2dOp.padding_typeAttr(), conv2dOp.act_funcAttr());

    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// Lower ZHigh BatchNorm to ZLow BatchNorm
//===----------------------------------------------------------------------===//
struct ZHighToZLowBatchNormOpLowering : public ConversionPattern {
  ZHighToZLowBatchNormOpLowering(TypeConverter &typeConverter, MLIRContext *ctx)
      : ConversionPattern(typeConverter,
            mlir::ZHighBatchNormOp::getOperationName(), 1, ctx) {}
  LogicalResult matchAndRewrite(Operation *op, ArrayRef<Value> operands,
      ConversionPatternRewriter &rewriter) const final {
    Location loc = op->getLoc();
    ZHighBatchNormOpAdaptor operandAdaptor(operands);
    Value input = operandAdaptor.input();

    // Convert ZTensor type to MemRefType.
    ZMemRefType zMemRefType =
        convertZTensorToMemRefType(rewriter, *op->result_type_begin());

    // Allocate a buffer for the result MemRef.
    MemRefBoundsIndexCapture inputBounds(input);
    IndexExprScope scope(&rewriter, loc);
    SmallVector<IndexExpr, 4> dims;
    inputBounds.getDimList(dims);
    Value alloc = insertAllocAndDeallocZMemRef(zMemRefType, dims, op, rewriter);

    // Get the original shape before it is vanished by lower passes.
    Value shape = insertShapeMemRefI64(rewriter, loc, dims);

    rewriter.create<ZLowBatchNormOp>(loc, operandAdaptor.input(),
        operandAdaptor.a(), operandAdaptor.b(), shape, alloc);
    rewriter.replaceOp(op, alloc);
    return success();
  }
};

//===----------------------------------------------------------------------===//
// ZHigh to ZLow Lowering Pass
//===----------------------------------------------------------------------===//

namespace {
/// Include the patterns defined in the Declarative Rewrite framework.
#include "Conversion/ZHighToZLow/ZHighToZLow.inc"

struct ZHighToZLowLoweringPass
    : public PassWrapper<ZHighToZLowLoweringPass, OperationPass<ModuleOp>> {

  StringRef getArgument() const override { return "convert-zhigh-to-zlow"; }

  StringRef getDescription() const override {
    return "Lower ZHigh ops to ZLow ops.";
  }

  // Make sure that we have a valid default constructor and copy
  // constructor to make sure that the options are initialized properly.
  ZHighToZLowLoweringPass() = default;
  ZHighToZLowLoweringPass(const ZHighToZLowLoweringPass &pass)
      : PassWrapper<ZHighToZLowLoweringPass, OperationPass<ModuleOp>>() {}
  ZHighToZLowLoweringPass(bool emitDealloc, bool enableTiling) {
    this->emitDealloc = emitDealloc;
    this->enableTiling = enableTiling;
  }
  ZHighToZLowLoweringPass(int optLevel)
      : ZHighToZLowLoweringPass(
            /*emitDealloc=*/false, /*enableTiling=*/optLevel >= 3) {}

  void runOnOperation() final;

public:
  Option<bool> emitDealloc{*this, "emit-dealloc",
      llvm::cl::desc("Emit dealloc for allocated memrefs or not."),
      llvm::cl::init(false)};
  Option<bool> enableTiling{*this, "enable-tiling",
      llvm::cl::desc("Enable loop tiling and unrolling optimizations"),
      llvm::cl::init(false)};
};
} // end anonymous namespace.

//===----------------------------------------------------------------------===//
// Type converter. Extended from KrnlTypeConvert to support new element types in
// ZHigh Dialect.

class ZHighTypeConverter : public KrnlTypeConverter {
public:
  using KrnlTypeConverter::KrnlTypeConverter;

  ZHighTypeConverter() : KrnlTypeConverter() {}

  ZHighTypeConverter(OpBuilder &builder) : KrnlTypeConverter() {
    // The order of type conversion is important: later ones are tried earlier.
    addConversion([&](TensorType tensorType) {
      assert(tensorType.cast<ShapedType>().hasRank() &&
             "expected only ranked shapes");
      if (tensorType.cast<RankedTensorType>().getEncoding()) {
        ZMemRefType zMemRefType =
            convertZTensorToMemRefType(builder, tensorType);
        return zMemRefType.value;
      } else if (tensorType.getElementType().isa<ONNXStringType>()) {
        Type elementType = ONNXStringType::get(tensorType.getContext());
        return MemRefType::get(tensorType.getShape(), elementType);
      }
      return MemRefType::get(
          tensorType.getShape(), tensorType.getElementType());
    });
  }
};

void ZHighToZLowLoweringPass::runOnOperation() {
  ModuleOp module = getOperation();

  // Set up whether emitting dealloc for allocated memrefs or not.
  ONNXToKrnl_gEmitDealloc = emitDealloc;

  // The first thing to define is the conversion target. This will define the
  // final target for this lowering.
  ConversionTarget target(getContext());

  // We define the specific operations, or dialects, that are legal targets for
  // this lowering.
  target.addLegalDialect<ZLowDialect, KrnlOpsDialect, AffineDialect,
      arith::ArithmeticDialect, StandardOpsDialect, linalg::LinalgDialect,
      math::MathDialect, memref::MemRefDialect, AffineDialect,
      arith::ArithmeticDialect>();

  // If `emitDealloc` is turned off, make sure we don't have buffer deallocation
  // at this level. Will use MLIR buffer-deallocation for this purpose instead.
  if (!ONNXToKrnl_gEmitDealloc)
    target.addIllegalOp<mlir::memref::DeallocOp>();

  // ZHigh to ZLow operation lowering.
  RewritePatternSet patterns(&getContext());

  // Convert types to legal types for the Krnl dialect.
  OpBuilder builder(module->getContext());
  ZHighTypeConverter typeConverter(builder);
  target.addDynamicallyLegalOp<FuncOp>([&](FuncOp op) {
    // FuncOp is legal only if types have been converted to Std types.
    return typeConverter.isSignatureLegal(op.getType());
  });

  target.addDynamicallyLegalOp<CallOp>([&](CallOp op) {
    // CallOp is legal only if types have been converted to Std types.
    return typeConverter.isLegal(op);
  });

  // Operations that are legal only if types are not tensors.
  target.addDynamicallyLegalOp<mlir::ReturnOp>([&](Operation *op) {
    return llvm::none_of(op->getOperandTypes(),
        [](Type type) { return type.isa<TensorType>(); });
  });

  // ONNX ops to Krnl.
  MLIRContext *ctx = &getContext();
  populateONNXToKrnlConversionPattern(
      patterns, typeConverter, ctx, enableTiling);
  // ONNX ops to ZHigh.
  populateWithGenerated(patterns);
  patterns.insert<ZHighToZLowStickifiedConstantOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowStickOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowStickForLSTMOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowStickForGRUOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowUnstickOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowBinaryOpLowering<ZHighAddOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowBinaryOpLowering<ZHighSubOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowBinaryOpLowering<ZHighMulOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowBinaryOpLowering<ZHighDivOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowBinaryOpLowering<ZHighMinOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowBinaryOpLowering<ZHighMaxOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowUnaryOpLowering<ZHighLogOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowUnaryOpLowering<ZHighExpOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowUnaryOpLowering<ZHighReluOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowUnaryOpLowering<ZHighTanhOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowUnaryOpLowering<ZHighSigmoidOp>>(
      typeConverter, ctx);
  patterns.insert<ZHighToZLowSoftmaxOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowMeanReduce2DOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowMatMulOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowLSTMOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowGRUOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowBatchNormOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowConv2DOpLowering>(typeConverter, ctx);
  patterns.insert<ZHighToZLowPool2DOpLowering<ZHighMaxPool2DOp,
      ZHighMaxPool2DOpAdaptor, ZLowMaxPool2DOp>>(typeConverter, ctx);
  patterns.insert<ZHighToZLowPool2DOpLowering<ZHighAvgPool2DOp,
      ZHighAvgPool2DOpAdaptor, ZLowAvgPool2DOp>>(typeConverter, ctx);

  // With the target and rewrite patterns defined, we can now attempt the
  // conversion. The conversion will signal failure if any of our `illegal`
  // operations were not converted successfully.
  if (failed(applyPartialConversion(module, target, std::move(patterns))))
    signalPassFailure();
}

std::unique_ptr<Pass> mlir::createZHighToZLowPass() {
  return std::make_unique<ZHighToZLowLoweringPass>();
}

std::unique_ptr<Pass> mlir::createZHighToZLowPass(int optLevel) {
  return std::make_unique<ZHighToZLowLoweringPass>(optLevel);
}

std::unique_ptr<Pass> mlir::createZHighToZLowPass(
    bool emitDealloc, bool enableTiling) {
  return std::make_unique<ZHighToZLowLoweringPass>(emitDealloc, enableTiling);
}