; Constructed from the owned visible_pointer_dispatch AIR pattern; the group attachment is a test input.
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

define i32 @grouped_add(i32 %x) {
  %y = add i32 %x, 5
  ret i32 %y
}

define i32 @grouped_mul(i32 %x) {
  %y = mul i32 %x, 3
  ret i32 %y
}

define void @grouped_pointer(ptr addrspace(1) %out, i32 %tid) {
  %low = and i32 %tid, 1
  %odd = icmp ne i32 %low, 0
  %function = select i1 %odd, ptr @grouped_add, ptr @grouped_mul
  %value = call i32 %function(i32 %tid), !air.function_groups !10
  %index = zext i32 %tid to i64
  %destination = getelementptr i32, ptr addrspace(1) %out, i64 %index
  store i32 %value, ptr addrspace(1) %destination, align 4
  ret void
}

!air.kernel = !{!0}
!air.visible = !{!5, !8}
!air.version = !{!11}
!air.language_version = !{!12}
!0 = !{ptr @grouped_pointer, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!4 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!5 = !{ptr @grouped_add, !6, !7}
!6 = !{!9}
!7 = !{!13}
!8 = !{ptr @grouped_mul, !6, !7}
!9 = !{!"air.visible_output", !"air.arg_type_name", !"uint"}
!10 = !{!"choice"}
!11 = !{i32 2, i32 8, i32 0}
!12 = !{!"Metal", i32 4, i32 0, i32 0}
!13 = !{i32 0, !"air.visible_input", !"air.arg_type_name", !"uint", !"air.arg_name", !"x"}
