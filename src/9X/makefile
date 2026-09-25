!ifdef MASTER_MAKE
BUILD_BITS=32
BUILD_TYPE=block
!INCLUDE $(DDKROOT)\master.mk
!endif

INCLUDE =.;$(INCLUDE)

DEVICE          = ahcint9x
OBJS            = ahcimain.obj ahcisatl.obj

######### Definitions for the debug level #########

!ifdef DEBUG

DDEBUG =-DDEBLEVEL=1 -DDEBUG
CFLAGS =$(CFLAGS) -Oy1

!else

DDEBUG =-DDEBLEVEL=0

!endif

########## Definitions for C compiler #############

CL      = cl
CFLAGS  = -nologo -Zp4 -Gs -c -DIS_32 -D_X86_ -Zl -Gz -DDBG $(CFLAGS)
CFLAGS  = $(CFLAGS) $(DDEBUG)

########## Definitions for linker #############

LINK 	 = link

########## Base inference rules ###################

#       32 bit C compiler inference rules

.c.obj:
	set CL=$(CFLAGS)
	$(CL) -Fo$*.obj $<

############### VxD device section ####################

$(DEVICE).sym: $(DEVICE).map
	mapsym -s -o .\$(DEVICE).sym .\$(DEVICE).map > nul

$(DEVICE).map: $(DEVICE).mpd

$(DEVICE).mpd : $(OBJS) $(LIBS) $(STUB)
	$(LINK) @$(DEVICE).lnk

clean:
	-@del *.obj
	-@del *.mpd
	-@del *.map
	-@del *.pdb
	-@del *.sym