; RUN: llc -mtriple=mipsel-linux-gnu -march=mipsel -mcpu=mips16 -soft-float -mips16-hard-float -relocation-model=pic < %s | FileCheck %s -check-prefix=pic

declare void @llvm.trap()

; Function Attrs: nounwind optsize readnone
define i32 @main()  {
entry:
  call void @llvm.trap()
  unreachable
; pic: break 0
  ret i32 0
}

