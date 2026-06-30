# Memory debugging helpers

define pmm-stats
  printf "Free frames: %llu\n", pmm_get_free_frames()
  printf "Total usable: %llu\n", pmm_get_usable_frames()
end

define show-vma
  # Usage: show-vma <tcb_ptr>
  set $tcb = (tcb_t *)$arg0
  set $vma = $tcb->vma_list
  while $vma != 0
    printf "VMA [%016lx - %016lx) flags=%08x\n", $vma->va_start, $vma->va_end, $vma->flags
    set $vma = $vma->next
  end
end

define show-ofd
  # Usage: show-ofd <fd_table_ptr> <max_fds>
  set $fdt = $arg0
  set $max = $arg1
  set $i = 0
  while $i < $max
    if $fdt[$i] != 0
      printf "fd[%d] = ", $i
      print *$fdt[$i]
    end
    set $i = $i + 1
  end
end
