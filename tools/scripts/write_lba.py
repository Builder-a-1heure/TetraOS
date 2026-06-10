import sys
import struct

def write_lba(img_file, bin_file, lba_start, num_sectors=0):
    try:
        with open(img_file, 'r+b') as img, open(bin_file, 'rb') as bin:
            img.seek(lba_start * 512)
            data = bin.read()
            
            # Vérifier taille pour le MBR
            if lba_start == 0 and len(data) > 512:
                raise ValueError("Le bootloader dépasse 512 octets")
            
            img.write(data)
            
            # Remplir le reste du secteur si besoin
            remaining = 512 - len(data)
            if remaining > 0:
                img.write(b'\0' * remaining)
            
            # Pour les secteurs supplémentaires
            if num_sectors > 1:
                total_sectors = num_sectors * 512
                remaining = total_sectors - len(data)
                if remaining > 0:
                    img.write(b'\0' * remaining)
        
        return True
    except Exception as e:
        print(f"Erreur: {e}")
        return False

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print("Usage: python write_lba.py <image> <file> <lba> [<sectors>]")
        sys.exit(1)
        
    sectors = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    if not write_lba(sys.argv[1], sys.argv[2], int(sys.argv[3]), sectors):
        sys.exit(1)
    sys.exit(0)