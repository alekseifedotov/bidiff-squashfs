A=$(HOME)/fw/main/ready-to-sign/bootloader/
B=$(HOME)/fw/main/ready-to-sign/bootloader/

LAYERS=ai_layer.img  base_layer.img cuda_layer.img cache_layer.img   lft_layer.img  packages_layer.img  security_layer.img  software_layer.img  system_layer.img

# TODO fir cuda layer
#
test: $(LAYERS:.img=.reconsitituted.img.checked)

%.patch: $(A)/%.img $(B)/%.img
	cargo run diff --method zstd $(A)/$*.img $(B)/$*.img $@
	echo "Patch size: " $$(stat -c %s $@)

%.reconsitituted.img: $(A)/%.img %.patch
	cargo run patch --method zstd $(A)/$*.img $*.patch $@

%.reconsitituted.img.checked: %.reconsitituted.img $(A)/%.img
	diff $^
	touch $@

.SECONDARY: %.patch
