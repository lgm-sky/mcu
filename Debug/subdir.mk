################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../cycx3_uvc.c \
../cycx3_uvcdscr.c \
../cyfx3hid_dscr.c \
../cyfxtx.c 

S_UPPER_SRCS += \
../cyfx_gcc_startup.S 

OBJS += \
./cycx3_uvc.o \
./cycx3_uvcdscr.o \
./cyfx3hid_dscr.o \
./cyfx_gcc_startup.o \
./cyfxtx.o 

C_DEPS += \
./cycx3_uvc.d \
./cycx3_uvcdscr.d \
./cyfx3hid_dscr.d \
./cyfxtx.d 

S_UPPER_DEPS += \
./cyfx_gcc_startup.d 


# Each subdirectory must supply rules for building sources it contributes
%.o: ../%.c
	@echo 'Building file: $<'
	@echo 'Invoking: Cross ARM C Compiler'
	arm-none-eabi-gcc -mcpu=arm926ej-s -marm -mthumb-interwork -O0 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -Wall  -g3 -D__CYU3P_TX__=1 -I"E:\lgm\cypress\EZ-USB FX3 SDK\1.3\/fw_lib/1_3_4/inc" -std=gnu11 -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '

%.o: ../%.S
	@echo 'Building file: $<'
	@echo 'Invoking: Cross ARM GNU Assembler'
	arm-none-eabi-gcc -mcpu=arm926ej-s -marm -mthumb-interwork -O0 -fmessage-length=0 -fsigned-char -ffunction-sections -fdata-sections -Wall  -g3 -x assembler-with-cpp -I"E:\lgm\cypress\EZ-USB FX3 SDK\1.3\/fw_lib/1_3_4/inc" -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" -c -o "$@" "$<"
	@echo 'Finished building: $<'
	@echo ' '


