#if DT_NODE_HAS_STATUS(DT_NODELABEL(psram), okay)
GROUP_START(PSRAM)

	SECTION_PROLOGUE(.ext_ram.bss, (NOLOAD),)
	{
		*(.bss_psram)
		*(".bss_psram.*")
	} GROUP_LINK_IN(PSRAM)

GROUP_END(PSRAM)
#endif