################################################################################
# Automatically-generated file. Do not edit!
################################################################################

-include ../makefile.init

RM := cs-rm -rf

# All of the sources participating in the build are defined here
-include sources.mk
-include subdir.mk
-include objects.mk

ifneq ($(MAKECMDGOALS),clean)
ifneq ($(strip $(C_DEPS)),)
-include $(C_DEPS)
endif
ifneq ($(strip $(ASM_DEPS)),)
-include $(ASM_DEPS)
endif
ifneq ($(strip $(S_UPPER_DEPS)),)
-include $(S_UPPER_DEPS)
endif
endif

-include ../makefile.defs

# Add inputs and outputs from these tool invocations to the build variables 

# All Target
all: Cx3UvcOV5640.elf secondary-outputs

# Tool invocations
Cx3UvcOV5640.elf: $(OBJS) $(USER_OBJS)
	@echo 'Building target: $@'
	@echo 'Invoking: Cross ARM C Linker'
	arm-none-eabi-gcc -mcpu=arm926ej-s -marm -mthumb-interwork -O0 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -Wall  -g3 -T "E:\lgm\cypress\EZ-USB FX3 SDK\1.3\/fw_build/fx3_fw/fx3.ld" -nostartfiles -Xlinker --gc-sections -L"E:\lgm\cypress\EZ-USB FX3 SDK\1.3\/fw_lib/1_3_4/fx3_debug" -L"E:\lgm\cypress\EZ-USB FX3 SDK\1.3\ARM GCC\/lib/gcc/arm-none-eabi/4.8.1" -L"E:\lgm\cypress\EZ-USB FX3 SDK\1.3\ARM GCC\/arm-none-eabi/lib" -Wl,-Map,"Cx3UvcOV5640.map" -Wl,-d -Wl,--no-wchar-size-warning -Wl,--entry,CyU3PFirmwareEntry -o "Cx3UvcOV5640.elf" $(OBJS) $(USER_OBJS) $(LIBS)
	@echo 'Finished building target: $@'
	@echo ' '
	$(MAKE) --no-print-directory post-build

# Other Targets
clean:
	-$(RM) $(OBJS)$(C_DEPS)$(ASM_DEPS)$(S_UPPER_DEPS) Cx3UvcOV5640.elf
	-@echo ' '

post-build:
	-@echo 'Generate boot-loadable binary image'
	-elf2img.exe -i Cx3UvcOV5640.elf -o Cx3UvcOV5640.img -v
	-@echo ' '

secondary-outputs:

.PHONY: all clean dependents
.SECONDARY: post-build

-include ../makefile.targets
