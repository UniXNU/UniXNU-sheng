; SPDX-License-Identifier: MIT
target triple = "air64_v28-apple-macosx26.0.0"
define void @compound(ptr addrspace(1) "air-buffer-no-alias" %in, ptr addrspace(1) "air-buffer-no-alias" %out) {
entry:
  %v0 = load <4 x float>, ptr addrspace(1) %in, align 16
  %p1 = getelementptr inbounds <4 x float>, ptr addrspace(1) %in, i64 1
  %v1 = load <4 x float>, ptr addrspace(1) %p1, align 16
  %h0 = fptrunc <4 x float> %v0 to <4 x half>
  %h1 = fptrunc <4 x float> %v1 to <4 x half>
  %x = extractelement <4 x half> %h0, i32 0
  %y = extractelement <4 x half> %h0, i32 1
  %t = extractelement <4 x half> %h0, i32 2
  %mix = call half @air.mix.f16(half %x, half %y, half %t)
  %a = shufflevector <4 x half> %h1, <4 x half> poison, <2 x i32> <i32 0, i32 1>
  %b = shufflevector <4 x half> %h1, <4 x half> poison, <2 x i32> <i32 2, i32 3>
  %dot = call half @air.dot.v2f16(<2 x half> %a, <2 x half> %b)
  %m = fpext half %mix to float
  %d = fpext half %dot to float
  store float %m, ptr addrspace(1) %out, align 4
  %o1 = getelementptr inbounds float, ptr addrspace(1) %out, i64 1
  store float %d, ptr addrspace(1) %o1, align 4
  ret void
}
declare half @air.mix.f16(half, half, half)
declare half @air.dot.v2f16(<2 x half>, <2 x half>)
!air.kernel = !{!0}
!air.version = !{!5}
!air.language_version = !{!6}
!0 = !{ptr @compound, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"in"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!5 = !{i32 2, i32 8, i32 0}
!6 = !{!"Metal", i32 4, i32 0, i32 0}
