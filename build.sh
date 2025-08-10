#!/bin/bash
#
# Compile script for Elaina Kernel
# Copyright (C) 2020-2021 Adithya R.

SECONDS=0 # builtin bash timer
ZIPNAME="Elaina_Milestria-Surya-$(date '+%Y%m%d-%H%M').zip"
TC_DIR="$(pwd)/tc/clang-20"
AK3_DIR="$(pwd)/android/AnyKernel3"
DEFCONFIG="surya_defconfig"

if test -z "$(git rev-parse --show-cdup 2>/dev/null)" &&
   head=$(git rev-parse --verify HEAD 2>/dev/null); then
	ZIPNAME="${ZIPNAME::-4}-$(echo $head | cut -c1-8).zip"
fi

export PATH="$TC_DIR/bin:$PATH"

sync_repo() {
    local dir=$1
    local repo_url=$2
    local branch=$3
    local update=$4

    if [ -d "$dir" ]; then
        if $update; then
            git -C "$dir" fetch origin --quiet
            git -C "$dir" checkout "$branch" --quiet

            LOCAL_COMMIT=$(git -C "$dir" rev-parse HEAD)
            REMOTE_COMMIT=$(git -C "$dir" rev-parse "origin/$branch")

            if [ "$LOCAL_COMMIT" != "$REMOTE_COMMIT" ]; then
                git -C "$dir" reset --quiet --hard "origin/$branch"
                LATEST_COMMIT=$(git -C "$dir" log -1 --oneline)
                echo -e "Updated $repo_url to: $LATEST_COMMIT\n" | tee -a "$dir/updates.txt"
            else
                echo "No changes found for $repo_url. Skipping update."
            fi
        fi
    else
        echo "Cloning $repo_url to $dir..."
        if ! git clone --quiet --depth=1 -b "$branch" "$repo_url" "$dir"; then
            echo "Cloning failed! Aborting..."
            exit 1
        fi
    fi
}

if [[ $1 = "-u" || $1 = "--update" ]]; then
    sync_repo $AK3_DIR "https://github.com/RavensVenix/AnyKernel3.git" "Milestria" true
    sync_repo $TC_DIR "https://bitbucket.org/rdxzv/clang-standalone.git" "20" true
	exit
else
    sync_repo $AK3_DIR "https://github.com/RavensVenix/AnyKernel3.git" "Milestria" false
    sync_repo $TC_DIR "https://bitbucket.org/rdxzv/clang-standalone.git" "20" false
fi

if [ ! -d "$AK3_DIR" ] || [ ! -d "$TC_DIR" ]; then
    echo "Error: Required directories are missing. Aborting the build process."
    exit 1
fi

if [[ $1 = "-r" || $1 = "--regen" ]]; then
	make $DEFCONFIG savedefconfig
	cp out/defconfig arch/arm64/configs/$DEFCONFIG
	echo -e "\nSuccessfully regenerated defconfig at $DEFCONFIG"
	exit
fi

if [[ $1 = "-rf" || $1 = "--regen-full" ]]; then
	make $DEFCONFIG
	cp out/.config arch/arm64/configs/$DEFCONFIG
	echo -e "\nSuccessfully regenerated full defconfig at $DEFCONFIG"
	exit
fi

CLEAN_BUILD=false
KSU_NEXT=false
SUKI_SU_NON_GKI=false
SUKI_SU_SUSFS=false
RSUNTK_KSU=false

for arg in "$@"; do
	case $arg in
		-c|--clean)
			CLEAN_BUILD=true
			;;
		--sukisu-susfs)
			SUKI_SU_SUSFS=true
			;;
		--sukisu)
			SUKI_SU_NON_GKI=true
			;;
		--ksun)
			KSU_NEXT=true
			;;
		--rsuntk)
			RSUNTK_KSU=true
			;;
		*)
			echo "Unknown argument: $arg"
			exit 1
			;;
	esac
done

if $SUKI_SU_NON_GKI; then
    echo "Building With SukiSU-Ultra Support."
	curl -LSs "https://raw.githubusercontent.com/SukiSU-Ultra/SukiSU-Ultra/main/kernel/setup.sh" | bash -s nongki
fi

if $SUKI_SU_SUSFS; then
    echo "Building With SukiSU-Ultra + SuSFS Support."
	curl -LSs "https://raw.githubusercontent.com/SukiSU-Ultra/SukiSU-Ultra/main/kernel/setup.sh" | bash -s susfs-main
fi

if $KSU_NEXT; then
    echo "Building With KernelSU-Next Support."
	curl -LSs "https://raw.githubusercontent.com/rifsxd/KernelSU-Next/next-susfs/kernel/setup.sh" | bash -s next-susfs
fi

if $RSUNTK_KSU; then
    echo "Building With Rsuntk KSU + SuSFS Support."
	curl -LSs "https://raw.githubusercontent.com/rsuntk/KernelSU/main/kernel/setup.sh" | bash -s c813c2e4a79a7e13b75d66e939506103c4f2e377
fi

if $CLEAN_BUILD; then
	echo "Cleaning output directory..."
	rm -rf out
fi

echo -e "\nStarting compilation...\n"
make $DEFCONFIG
make -j$(nproc --all) LLVM=1 Image.gz dtb.img dtbo.img 2> >(tee log.txt >&2) || exit $?

kernel="out/arch/arm64/boot/Image.gz"
dtb="out/arch/arm64/boot/dtb.img"
dtbo="out/arch/arm64/boot/dtbo.img"

if [ -f "$kernel" ] && [ -f "$dtb" ] && [ -f "$dtbo" ]; then
	echo -e "\nKernel compiled successfully! Zipping up...\n"
	cp -r $AK3_DIR AnyKernel3
	cp $kernel $dtb $dtbo AnyKernel3
	cd AnyKernel3
	git checkout Milestria &> /dev/null
	zip -r9 "../$ZIPNAME" * -x .git modules\* patch\* ramdisk\* README.md *placeholder
	cd ..
	rm -rf AnyKernel3
	echo -e "\nCompleted in $((SECONDS / 60)) minute(s) and $((SECONDS % 60)) second(s) !"
	echo "Zip: $ZIPNAME"
else
	echo -e "\nCompilation failed!"
	exit 1
fi
