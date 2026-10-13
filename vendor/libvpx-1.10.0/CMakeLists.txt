# libvpx 1.10.0 (the version WowClassic-clean.exe links: "WebM Project VP9 Decoder v1.10.0"),
# only the VP9 decoder, generic C. config/ is what libvpx's own configure and rtcd.pl wrote for
#   configure --target=generic-gnu --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder
#     --disable-postproc --disable-vp9-postproc --enable-multithread (and no examples/tools/tests)
# The reference also has the x86 SIMD paths; this build does not (no assembler in the toolchain).
add_library(vpx STATIC
    vpx/src/vpx_decoder.c
    vpx/src/vpx_codec.c
    vpx/src/vpx_image.c
    vpx_mem/vpx_mem.c
    vpx_scale/generic/vpx_scale.c
    vpx_scale/generic/yv12config.c
    vpx_scale/generic/yv12extend.c
    vpx_scale/generic/gen_scalers.c
    vpx_scale/vpx_scale_rtcd.c
    vpx_dsp/prob.c
    vpx_dsp/bitreader.c
    vpx_dsp/bitreader_buffer.c
    vpx_dsp/intrapred.c
    vpx_dsp/add_noise.c
    vpx_dsp/deblock.c
    vpx_dsp/vpx_convolve.c
    vpx_dsp/loopfilter.c
    vpx_dsp/inv_txfm.c
    vpx_dsp/vpx_dsp_rtcd.c
    vpx_util/vpx_thread.c
    vpx_util/vpx_write_yuv_frame.c
    vp9/vp9_iface_common.c
    vp9/common/vp9_alloccommon.c
    vp9/common/vp9_blockd.c
    vp9/common/vp9_entropy.c
    vp9/common/vp9_entropymode.c
    vp9/common/vp9_entropymv.c
    vp9/common/vp9_frame_buffers.c
    vp9/common/vp9_idct.c
    vp9/common/vp9_filter.c
    vp9/common/vp9_pred_common.c
    vp9/common/vp9_rtcd.c
    vp9/common/vp9_scale.c
    vp9/common/vp9_seg_common.c
    vp9/common/vp9_tile_common.c
    vp9/common/vp9_loopfilter.c
    vp9/common/vp9_thread_common.c
    vp9/common/vp9_mvref_common.c
    vp9/common/vp9_quant_common.c
    vp9/common/vp9_reconinter.c
    vp9/common/vp9_reconintra.c
    vp9/common/vp9_common_data.c
    vp9/common/vp9_scan.c
    vp9/vp9_dx_iface.c
    vp9/decoder/vp9_decodemv.c
    vp9/decoder/vp9_decodeframe.c
    vp9/decoder/vp9_detokenize.c
    vp9/decoder/vp9_decoder.c
    vp9/decoder/vp9_dsubexp.c
    vp9/decoder/vp9_job_queue.c
    config/vpx_config.c
)
target_include_directories(vpx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/config)
if(MSVC)
    # Upstream code, built as is.
    target_compile_options(vpx PRIVATE /W0)
endif()
